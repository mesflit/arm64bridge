// SPDX-License-Identifier: GPL-2.0-or-later
//
// The dynamic linker API (dlopen, dlsym, dladdr, ...), compatible with Android before 5.0.
//
// Back then a dlopen() handle was a pointer to the linker's own record of the library (struct soinfo).
// Some games ship a copy of that linker (Glu's Unity games load their real libraries with it) and look
// symbols up directly in such records: in the ELF hash table they point to. So every handle given out
// here is such a record in guest memory, with the old layout and a hash table of the library's symbols:
//
//   0x00 name[128]   0x8c base   0x90 size   0xa4 next   0xa8 flags   0xac strtab   0xb0 symtab
//   0xb4 nbucket     0xb8 nchain 0xbc bucket 0xc0 chain
//
// Libraries that such a built-in linker maps itself may need functions the bridge does not have; they
// get placeholders (see add_imports_of_foreign_images) so that linking succeeds and only a real call
// reports the missing function.

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "elf_loader.h"
#include "guest_mem.h"
#include "thunk.h"

namespace a64b {

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()

namespace {

// ------------------------------------------------------------------ dlerror

thread_local std::string t_dlerror;
thread_local bool t_dlerror_set = false;
u32 g_dlerror_buf = 0;

void set_dlerror(const std::string& e) {
    t_dlerror = e;
    t_dlerror_set = true;
}

// ------------------------------------------------------------------ ELF symbol tables in guest memory

using SymbolList = std::vector<std::pair<std::string, u32>>;  // name, absolute guest address

struct SymbolTable {
    u32 strtab = 0, symtab = 0, nbucket = 0, nchain = 0, bucket = 0, chain = 0;
};

u32 elf_hash(const char* name) {
    u32 h = 0, g;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        h = (h << 4) + *p;
        g = h & 0xf0000000u;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

// Builds strtab, symtab (Elf32_Sym: name, value, size, info, other, shndx) and the ELF hash table.
SymbolTable build_table(const SymbolList& syms) {
    SymbolTable t;
    const u32 n = (u32)syms.size();
    size_t str_size = 1;
    for (auto& kv : syms) str_size += kv.first.size() + 1;
    t.strtab = g_calloc(1, (u32)str_size);
    t.symtab = g_calloc(n + 1, 16);
    t.nbucket = n | 1;
    t.nchain = n + 1;
    t.bucket = g_calloc(t.nbucket, 4);
    t.chain = g_calloc(t.nchain, 4);
    u32 str_off = 1;
    for (u32 i = 0; i < n; i++) {
        const std::string& name = syms[i].first;
        memcpy(g2h<char>(t.strtab + str_off), name.c_str(), name.size() + 1);
        const u32 sym = t.symtab + (i + 1) * 16;
        *g2h<u32>(sym) = str_off;
        *g2h<u32>(sym + 4) = syms[i].second;  // the record's base is 0: values are addresses
        *g2h<u8>(sym + 12) = (1 << 4) | 2;     // STB_GLOBAL, STT_FUNC
        *g2h<u16>(sym + 14) = 1;               // defined
        const u32 b = elf_hash(name.c_str()) % t.nbucket;
        *g2h<u32>(t.chain + (i + 1) * 4) = *g2h<u32>(t.bucket + b * 4);
        *g2h<u32>(t.bucket + b * 4) = i + 1;
        str_off += (u32)name.size() + 1;
    }
    return t;
}

// ------------------------------------------------------------------ old-style handles (soinfo)

std::recursive_mutex g_mu;

struct Handle {
    Module* module = nullptr;  // nullptr: a library implemented by the bridge
};
std::unordered_map<u32, Handle> g_handles;           // soinfo address -> what it stands for
std::unordered_map<std::string, u32> g_system_soinfos;  // library name -> soinfo
std::unordered_map<Module*, u32> g_module_soinfos;
SymbolTable g_system_table;

void write_table(u32 si, const SymbolTable& t) {
    *g2h<u32>(si + 0xac) = t.strtab;
    *g2h<u32>(si + 0xb0) = t.symtab;
    *g2h<u32>(si + 0xb4) = t.nbucket;
    *g2h<u32>(si + 0xb8) = t.nchain;
    *g2h<u32>(si + 0xbc) = t.bucket;
    *g2h<u32>(si + 0xc0) = t.chain;
}

u32 new_soinfo(const std::string& name, const SymbolTable& t, Module* m) {
    u32 si = g_calloc(1, 1024);
    snprintf(g2h<char>(si), 128, "%s", name.c_str());
    *g2h<u32>(si + 0xa8) = 1;  // FLAG_LINKED
    write_table(si, t);
    g_handles[si] = Handle{m};
    return si;
}

// ------------------------------------------------------------------ placeholders for foreign libraries

std::unordered_map<std::string, u32> g_placeholders;  // missing symbol -> stub or zeroed data
std::vector<u32> g_scanned_images;

bool provided(const std::string& name) { return thunk_exists(name) || loader_lookup(name) != 0; }

// Reads the undefined symbols of an ELF image that is mapped in guest memory (header at `a`) and gives
// every one the bridge lacks a placeholder. Returns how many were added.
u32 add_placeholders_for_image(u32 a) {
    const u32 page = mem_page_size();
    const u32 phoff = *g2h<u32>(a + 28), phnum = *g2h<u16>(a + 44);
    if (phnum > 32 || phoff > page - 32 * phnum) return 0;
    u32 bias = 0, dyn = 0;
    bool have_load = false;
    for (u32 i = 0; i < phnum; i++) {
        const u32* ph = g2h<u32>(a + phoff + i * 32);
        if (ph[0] == 1 && !have_load) {  // the first PT_LOAD maps the header
            bias = a - (ph[2] & ~(page - 1));
            have_load = true;
        }
        if (ph[0] == 2) dyn = ph[2];  // PT_DYNAMIC
    }
    if (!have_load || !dyn) return 0;
    // A linker may or may not have relocated the dynamic section in place.
    auto addr = [&](u32 v) { return v >= bias && mem_is_mapped(v, 4) && v - bias < 0x10000000u ? v : bias + v; };
    u32 d = bias + dyn, symtab = 0, strtab = 0, hash = 0;
    for (int i = 0; i < 256 && mem_is_mapped(d + i * 8, 8); i++) {
        const u32 tag = *g2h<u32>(d + i * 8), val = *g2h<u32>(d + i * 8 + 4);
        if (tag == 0) break;
        if (tag == 4) hash = addr(val);    // DT_HASH
        if (tag == 5) strtab = addr(val);  // DT_STRTAB
        if (tag == 6) symtab = addr(val);  // DT_SYMTAB
    }
    if (!symtab || !strtab || !hash || !mem_is_mapped(hash, 8)) return 0;
    const u32 nsyms = *g2h<u32>(hash + 4);
    u32 added = 0;
    for (u32 i = 1; i < nsyms && mem_is_mapped(symtab + i * 16, 16); i++) {
        const u32 sym = symtab + i * 16, name_off = *g2h<u32>(sym);
        const u8 info = *g2h<u8>(sym + 12);
        if (*g2h<u16>(sym + 14) != 0 || !name_off || (info >> 4) == 2) continue;  // defined, unnamed, weak
        if (!mem_is_mapped(strtab + name_off, 1)) continue;
        std::string name = g2h<char>(strtab + name_off);
        if (name.empty() || provided(name) || g_placeholders.count(name)) continue;
        // Data gets a zeroed block, functions a stub that reports the call and returns 0.
        g_placeholders[name] = (info & 0xf) == 1 ? g_calloc(1, 256) : thunk_missing(name);
        added++;
    }
    return added;
}

// Looks for ELF images in guest memory that our loader did not load (mapped by a linker inside the
// app) and adds placeholders for what they import. Returns true if any were added.
bool add_imports_of_foreign_images() {
    bool added = false;
    for (auto& r : mem_used_regions()) {
        for (u32 a = r.first; a + 64 <= r.first + r.second; a += mem_page_size()) {
            const u8* h = g2h<u8>(a);
            if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 1) continue;
            if (*g2h<u16>(a + 16) != 3 || *g2h<u16>(a + 18) != 40) continue;  // ET_DYN, EM_ARM
            if (loader_module_for(a)) continue;                                 // loaded by us
            if (std::find(g_scanned_images.begin(), g_scanned_images.end(), a) != g_scanned_images.end()) continue;
            g_scanned_images.push_back(a);
            if (u32 n = add_placeholders_for_image(a)) {
                std::string list;
                for (auto& kv : g_placeholders) list += (list.empty() ? "" : " ") + kv.first;
                LOGW("library loaded by the app's own linker at 0x%08x needs %u functions the bridge does not have "
                     "(placeholders so far: %s)", a, n, list.c_str());
                added = true;
            }
        }
    }
    return added;
}

// ------------------------------------------------------------------ handles for libraries

// The handle of a library implemented by the bridge. All of them share one table: every bridge symbol
// and the placeholders. It is rebuilt when a foreign library brought new placeholders.
u32 system_handle(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (add_imports_of_foreign_images() || !g_system_table.symtab) {
        SymbolList syms = thunk_resolve_all();
        for (auto& kv : g_placeholders) syms.emplace_back(kv.first, kv.second);
        g_system_table = build_table(syms);
        for (auto& kv : g_system_soinfos) write_table(kv.second, g_system_table);
        LOGI("dlopen: symbol table of the bridge for old-style handles: %zu symbols", syms.size());
    }
    auto it = g_system_soinfos.find(name);
    if (it != g_system_soinfos.end()) return it->second;
    return g_system_soinfos[name] = new_soinfo(name, g_system_table, nullptr);
}

// The handle of a library our loader loaded: its exports.
u32 module_handle(Module* m) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_module_soinfos.find(m);
    if (it != g_module_soinfos.end()) return it->second;
    SymbolList syms(m->exports.begin(), m->exports.end());
    u32 si = new_soinfo(m->name, build_table(syms), m);
    *g2h<u32>(si + 0x8c) = 0;  // symbol values are absolute (base 0)
    *g2h<u32>(si + 0x90) = m->size;
    return g_module_soinfos[m] = si;
}

// What a handle stands for: found = false for something that is not one of our handles.
struct Lookup {
    bool found = false;
    Module* module = nullptr;
};
Lookup lookup_handle(u32 h) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_handles.find(h);
    if (it != g_handles.end()) return {true, it->second.module};
    for (Module* m : loader_modules())  // handles given out as the load address (older behaviour)
        if (m->base == h) return {true, m};
    return {};
}

u32 placeholder(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_placeholders.find(name);
    return it == g_placeholders.end() ? 0 : it->second;
}

}  // namespace

void register_dl_thunks() {
    reg("dlopen", +[](const char* name, int) -> u32 {
        if (!name) return system_handle("");
        std::string n = name;
        size_t slash = n.rfind('/');
        if (slash != std::string::npos) n = n.substr(slash + 1);
        if (loader_is_system_lib(n)) return system_handle(n);
        std::string err;
        Module* m = loader_load(n, &err);
        if (!m) {
            set_dlerror(err);
            return 0;
        }
        return module_handle(m);
    });
    reg("dlclose", +[](u32) -> int { return 0; });
    reg("dlsym", +[](u32 handle, const char* name) -> u32 {
        Lookup h = lookup_handle(handle);
        u32 a = h.module ? h.module->find(name) : 0;
        if (!a && !h.module) {  // a bridge library, RTLD_DEFAULT/RTLD_NEXT or an unknown handle
            if (!h.found) a = loader_lookup(name);
            if (!a) a = thunk_resolve(name);
            if (!a) a = placeholder(name);
        }
        if (!a) set_dlerror(std::string("undefined symbol: ") + name);
        return a;
    });
    reg("dlerror", +[]() -> u32 {
        if (!t_dlerror_set) return 0;
        t_dlerror_set = false;
        if (!g_dlerror_buf) g_dlerror_buf = g_calloc(1, 512);
        snprintf(g2h<char>(g_dlerror_buf), 512, "%s", t_dlerror.c_str());
        return g_dlerror_buf;
    });
    reg("dladdr", +[](u32 addr, u32* info) -> int {
        Module* m = loader_module_for(addr);
        if (!m) return 0;
        info[0] = guest_intern_string(m->name.c_str());
        info[1] = m->base;
        info[2] = 0;
        info[3] = 0;
        return 1;
    });

    // Exception unwinding tables of loaded libraries.
    auto find_exidx = +[](u32 pc, i32* count) -> u32 {
        Module* m = loader_module_for(pc);
        if (!m || !m->exidx) {
            if (count) *count = 0;
            return 0;
        }
        if (count) *count = (i32)m->exidx_count;
        return m->exidx;
    };
    reg("__gnu_Unwind_Find_exidx", find_exidx);
    reg("dl_unwind_find_exidx", find_exidx);
    RAW(dl_iterate_phdr) {
        ArgReader a(c);
        u32 cb = A32(), data = A32();
        u32 info = g_calloc(1, 32);
        u32 ret = 0;
        for (Module* m : loader_modules()) {
            if (!m->phdr) continue;
            u32* p = g2h<u32>(info);
            p[0] = m->bias;
            p[1] = guest_intern_string(m->name.c_str());
            p[2] = m->phdr;
            p[3] = m->phnum;  // Elf32_Half followed by padding
            ret = (u32)c.call(cb, {info, 16u, data});
            if (ret) break;
        }
        g_free(info);
        c.set_ret32(ret);
    });
}

}  // namespace a64b
