// SPDX-License-Identifier: GPL-2.0-or-later
#include "elf_loader.h"

#include <elf.h>

#include <algorithm>
#include <mutex>

#include "cpu.h"
#include "guest_mem.h"
#include "thunk.h"

#ifndef DT_RELR
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37
#endif
#define A_DT_ANDROID_REL 0x6000000f
#define A_DT_ANDROID_RELSZ 0x60000010
#define A_DT_ANDROID_RELR 0x6fffe000
#define A_DT_ANDROID_RELRSZ 0x6fffe001
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef R_ARM_IRELATIVE
#define R_ARM_IRELATIVE 160
#endif

namespace a64b {

namespace {

std::recursive_mutex g_load_mu;
std::vector<Module*> g_modules;
std::vector<LibProvider> g_providers;

// A view of an ELF image laid out by virtual address. "origin" is the host
// pointer that corresponds to vaddr 0 (may point before the buffer).
struct Dyn {
    const u8* origin = nullptr;
    u32 lo = 0, hi = 0;  // valid vaddr range
    const Elf32_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    u32 strsz = 0;
    u32 nsyms = 0;
    const Elf32_Rel* rel = nullptr;
    u32 relsz = 0;
    const Elf32_Rel* jmprel = nullptr;
    u32 jmprelsz = 0;
    const u32* relr = nullptr;
    u32 relrsz = 0;
    const u8* android_rel = nullptr;
    u32 android_relsz = 0;
    u32 init = 0, init_array = 0, init_arraysz = 0;
    bool symbolic = false;
    bool has_tls = false;
    std::vector<u32> needed_off;

    template <class T>
    const T* at(u32 vaddr, u32 len = sizeof(T)) const {
        if (vaddr < lo || (u64)vaddr + len > hi) return nullptr;
        return reinterpret_cast<const T*>(origin + vaddr);
    }
    const char* str(u32 off) const {
        if (!strtab || off >= strsz) return "";
        return strtab + off;
    }
};

bool check_header(const u8* data, size_t size, std::string& err, std::string* machine = nullptr) {
    if (size < sizeof(Elf32_Ehdr) || memcmp(data, ELFMAG, SELFMAG) != 0) {
        err = "not an ELF file";
        return false;
    }
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(data);
    if (machine) {
        if (data[EI_CLASS] == ELFCLASS64) {
            auto* eh64 = reinterpret_cast<const Elf64_Ehdr*>(data);
            *machine = eh64->e_machine == EM_AARCH64 ? "arm64" : eh64->e_machine == EM_X86_64 ? "x86_64" : "64-bit";
        } else {
            *machine = eh->e_machine == EM_ARM ? "arm" : eh->e_machine == EM_386 ? "x86" : "32-bit";
        }
    }
    if (data[EI_CLASS] != ELFCLASS32 || data[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_ARM) {
        err = "not a 32-bit ARM library";
        return false;
    }
    if (eh->e_type != ET_DYN) {
        err = "not a shared object";
        return false;
    }
    if (eh->e_phentsize != sizeof(Elf32_Phdr) || (u64)eh->e_phoff + (u64)eh->e_phnum * sizeof(Elf32_Phdr) > size) {
        err = "corrupt program headers";
        return false;
    }
    return true;
}

bool parse_dynamic(Dyn& d, u32 dyn_vaddr, u32 dyn_size, std::string& err) {
    u32 n = dyn_size / sizeof(Elf32_Dyn);
    auto* dyn = d.at<Elf32_Dyn>(dyn_vaddr, dyn_size);
    if (!dyn) {
        err = "dynamic section out of range";
        return false;
    }
    u32 symtab = 0, strtab = 0, hash = 0, gnu_hash = 0, rel = 0, jmprel = 0, relr = 0, arel = 0;
    for (u32 i = 0; i < n && dyn[i].d_tag != DT_NULL; i++) {
        u32 v = dyn[i].d_un.d_val;
        switch ((u32)dyn[i].d_tag) {
            case DT_SYMTAB: symtab = v; break;
            case DT_STRTAB: strtab = v; break;
            case DT_STRSZ: d.strsz = v; break;
            case DT_HASH: hash = v; break;
            case DT_GNU_HASH: gnu_hash = v; break;
            case DT_REL: rel = v; break;
            case DT_RELSZ: d.relsz = v; break;
            case DT_JMPREL: jmprel = v; break;
            case DT_PLTRELSZ: d.jmprelsz = v; break;
            case DT_PLTREL:
                if (v != DT_REL) {
                    err = "RELA PLT relocations are not valid for ARM";
                    return false;
                }
                break;
            case DT_RELR:
            case A_DT_ANDROID_RELR: relr = v; break;
            case DT_RELRSZ:
            case A_DT_ANDROID_RELRSZ: d.relrsz = v; break;
            case A_DT_ANDROID_REL: arel = v; break;
            case A_DT_ANDROID_RELSZ: d.android_relsz = v; break;
            case DT_INIT: d.init = v; break;
            case DT_INIT_ARRAY: d.init_array = v; break;
            case DT_INIT_ARRAYSZ: d.init_arraysz = v; break;
            case DT_SYMBOLIC: d.symbolic = true; break;
            case DT_FLAGS:
                if (v & DF_SYMBOLIC) d.symbolic = true;
                break;
            case DT_NEEDED: d.needed_off.push_back(v); break;
            default: break;
        }
    }
    d.strtab = d.at<char>(strtab, d.strsz);
    if (!d.strtab || !symtab) {
        err = "missing dynamic symbol/string table";
        return false;
    }
    if (hash) {
        auto* h = d.at<u32>(hash, 8);
        if (h) d.nsyms = h[1];
    } else if (gnu_hash) {
        auto* gh = d.at<u32>(gnu_hash, 16);
        if (gh) {
            u32 nbuckets = gh[0], symoffset = gh[1], bloom = gh[2];
            auto* buckets = d.at<u32>(gnu_hash + 16 + bloom * 4, nbuckets * 4);
            u32 maxsym = 0;
            if (buckets)
                for (u32 i = 0; i < nbuckets; i++) maxsym = std::max(maxsym, buckets[i]);
            if (maxsym < symoffset) {
                d.nsyms = symoffset;
            } else {
                u32 chains = gnu_hash + 16 + bloom * 4 + nbuckets * 4;
                u32 i = maxsym;
                for (;;) {
                    auto* ch = d.at<u32>(chains + (i - symoffset) * 4);
                    if (!ch) break;
                    if (*ch & 1) break;
                    i++;
                }
                d.nsyms = i + 1;
            }
        }
    }
    d.symtab = d.at<Elf32_Sym>(symtab, d.nsyms * sizeof(Elf32_Sym));
    if (!d.symtab) {
        err = "symbol table out of range";
        return false;
    }
    if (rel) d.rel = d.at<Elf32_Rel>(rel, d.relsz);
    if (jmprel) d.jmprel = d.at<Elf32_Rel>(jmprel, d.jmprelsz);
    if (relr) d.relr = d.at<u32>(relr, d.relrsz);
    if (arel) d.android_rel = d.at<u8>(arel, d.android_relsz);
    return true;
}

struct SlebReader {
    const u8* p;
    const u8* end;
    i32 next() {
        i32 value = 0;
        u32 shift = 0;
        u8 b = 0;
        do {
            if (p >= end) return 0;
            b = *p++;
            value |= (i32)((u32)(b & 0x7f) << shift);
            shift += 7;
        } while (b & 0x80);
        if (shift < 32 && (b & 0x40)) value |= -(i32)(1u << shift);
        return value;
    }
};

// Decodes Android packed relocations (APS2) into plain REL entries.
bool unpack_android_rel(const u8* data, u32 size, std::vector<Elf32_Rel>& out) {
    if (size < 4 || memcmp(data, "APS2", 4) != 0) return false;
    SlebReader r{data + 4, data + size};
    u32 count = (u32)r.next();
    u32 offset = (u32)r.next();
    u32 info = 0;
    out.reserve(count);
    while (out.size() < count) {
        u32 group_size = (u32)r.next();
        u32 flags = (u32)r.next();
        const bool by_info = flags & 1, by_delta = flags & 2, by_addend = flags & 4, has_addend = flags & 8;
        u32 delta = by_delta ? (u32)r.next() : 0;
        if (by_info) info = (u32)r.next();
        if (has_addend && by_addend) r.next();
        if (group_size == 0 || r.p >= r.end + 1) return false;
        for (u32 i = 0; i < group_size && out.size() < count; i++) {
            offset += by_delta ? delta : (u32)r.next();
            if (!by_info) info = (u32)r.next();
            if (has_addend && !by_addend) r.next();
            out.push_back(Elf32_Rel{offset, info});
        }
    }
    return true;
}

const char* const kSystemLibs[] = {"libc.so",        "libm.so",       "libdl.so",        "liblog.so",   "libstdc++.so",
                                   "libandroid.so",  "libEGL.so",     "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
                                   "libz.so",        "libOpenSLES.so", "libjnigraphics.so", "libOpenMAXAL.so",
                                   "libmediandk.so", "libvulkan.so",  "libcamera2ndk.so", "libaaudio.so", "libnativewindow.so"};

bool fetch(const std::string& name, std::vector<u8>& out) {
    for (auto& p : g_providers)
        if (p(name, out)) return true;
    return false;
}

std::string base_name(const std::string& path) {
    size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

u32 resolve_symbol(Module* self, const Dyn& d, u32 symidx, bool* weak_undef) {
    *weak_undef = false;
    if (symidx == 0 || symidx >= d.nsyms) return 0;
    const Elf32_Sym& s = d.symtab[symidx];
    std::string name = d.str(s.st_name);
    if (s.st_shndx != SHN_UNDEF) return self->bias + s.st_value;
    for (Module* m : g_modules) {
        if (m == self) continue;
        if (u32 a = m->find(name)) return a;
    }
    if (u32 a = thunk_resolve(name)) return a;
    if (ELF32_ST_BIND(s.st_info) == STB_WEAK) {
        *weak_undef = true;
        return 0;
    }
    if (ELF32_ST_TYPE(s.st_info) == STT_OBJECT) {
        LOGW("%s: unresolved data symbol %s (zero-filled placeholder)", self->name.c_str(), name.c_str());
        u32 a = g_calloc(1, s.st_size > 64 ? s.st_size : 64);
        thunk_register_data(name.c_str(), a);
        return a;
    }
    LOGW("%s: unresolved function %s", self->name.c_str(), name.c_str());
    return thunk_missing(name);
}

bool apply_rel(Module* m, const Dyn& d, const Elf32_Rel* rel, u32 count, std::string& err) {
    for (u32 i = 0; i < count; i++) {
        u32 type = ELF32_R_TYPE(rel[i].r_info), sym = ELF32_R_SYM(rel[i].r_info);
        u32 where = m->bias + rel[i].r_offset;
        if (where < m->base || where + 4 > m->base + m->size) {
            err = "relocation outside of image";
            return false;
        }
        u32* p = g2h<u32>(where);
        bool weak = false;
        switch (type) {
            case R_ARM_NONE: break;
            case R_ARM_RELATIVE: *p += m->bias; break;
            case R_ARM_ABS32: *p += resolve_symbol(m, d, sym, &weak); break;
            case R_ARM_REL32: *p += resolve_symbol(m, d, sym, &weak) - where; break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: *p = resolve_symbol(m, d, sym, &weak); break;
            case R_ARM_IRELATIVE: *p = (u32)Cpu::current().call(m->bias + *p, {}); break;
            case R_ARM_TLS_DTPMOD32:
            case R_ARM_TLS_DTPOFF32:
            case R_ARM_TLS_TPOFF32:
                LOGW("%s: ELF TLS relocation %u is not supported", m->name.c_str(), type);
                break;
            default:
                LOGW("%s: unknown relocation type %u", m->name.c_str(), type);
                break;
        }
    }
    return true;
}

Module* load_locked(const std::string& name, std::string& err) {
    for (Module* m : g_modules)
        if (m->name == name) return m;
    std::vector<u8> file;
    if (!fetch(name, file)) {
        err = "library " + name + " not found";
        return nullptr;
    }
    if (!check_header(file.data(), file.size(), err)) {
        err = name + ": " + err;
        return nullptr;
    }
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(file.data());
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(file.data() + eh->e_phoff);
    u32 lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if ((u64)ph[i].p_offset + ph[i].p_filesz > file.size() || ph[i].p_filesz > ph[i].p_memsz) {
            err = name + ": corrupt segment";
            return nullptr;
        }
        lo = std::min(lo, ph[i].p_vaddr & ~0xFFFu);
        hi = std::max(hi, (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~0xFFFu);
    }
    if (hi <= lo) {
        err = name + ": no loadable segments";
        return nullptr;
    }
    u32 base = page_alloc(hi - lo, true);
    if (!base) {
        err = name + ": out of guest memory";
        return nullptr;
    }
    auto* m = new Module();
    m->name = name;
    m->base = base;
    m->size = hi - lo;
    m->bias = base - lo;

    Dyn d;
    d.origin = g2h<u8>(m->bias);
    d.lo = lo;
    d.hi = hi;
    u32 dyn_vaddr = 0, dyn_size = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr& p = ph[i];
        switch (p.p_type) {
            case PT_LOAD:
                memcpy(g2h(m->bias + p.p_vaddr), file.data() + p.p_offset, p.p_filesz);
                if (p.p_offset <= eh->e_phoff && eh->e_phoff + eh->e_phnum * sizeof(Elf32_Phdr) <= p.p_offset + p.p_filesz) {
                    m->phdr = m->bias + p.p_vaddr + (eh->e_phoff - p.p_offset);
                    m->phnum = eh->e_phnum;
                }
                break;
            case PT_DYNAMIC:
                dyn_vaddr = p.p_vaddr;
                dyn_size = p.p_memsz;
                break;
            case PT_ARM_EXIDX:
                m->exidx = m->bias + p.p_vaddr;
                m->exidx_count = p.p_memsz / 8;
                break;
            case PT_TLS:
                if (p.p_memsz) LOGW("%s: uses ELF TLS, which is not supported yet", name.c_str());
                break;
            default: break;
        }
    }
    file.clear();
    file.shrink_to_fit();
    if (!dyn_size || !parse_dynamic(d, dyn_vaddr, dyn_size, err)) {
        if (err.empty()) err = "no dynamic section";
        err = name + ": " + err;
        page_free(m->base, m->size);
        delete m;
        return nullptr;
    }
    m->symbolic = d.symbolic;
    for (u32 i = 1; i < d.nsyms; i++) {
        const Elf32_Sym& s = d.symtab[i];
        if (s.st_shndx == SHN_UNDEF) continue;
        int bind = ELF32_ST_BIND(s.st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        m->exports.emplace(d.str(s.st_name), m->bias + s.st_value);
    }
    for (u32 off : d.needed_off) m->needed.push_back(d.str(off));
    g_modules.push_back(m);
    LOGI("loaded %s at 0x%08x-0x%08x (%zu exports)", name.c_str(), m->base, m->base + m->size, m->exports.size());
    frame_cap_scan(m);

    for (auto& dep : m->needed) {
        std::string dn = base_name(dep);
        if (loader_is_system_lib(dn)) continue;
        std::string derr;
        if (!load_locked(dn, derr)) LOGW("%s: dependency %s unavailable (%s)", name.c_str(), dn.c_str(), derr.c_str());
    }

    bool ok = true;
    if (d.relr) {
        u32 where = 0;
        for (u32 i = 0; i < d.relrsz / 4; i++) {
            u32 e = d.relr[i];
            if ((e & 1) == 0) {
                where = e;
                *g2h<u32>(m->bias + where) += m->bias;
                where += 4;
            } else {
                u32 off = where;
                for (u32 bits = e >> 1; bits; bits >>= 1, off += 4)
                    if (bits & 1) *g2h<u32>(m->bias + off) += m->bias;
                where += 31 * 4;
            }
        }
    }
    if (d.android_rel) {
        std::vector<Elf32_Rel> unpacked;
        if (!unpack_android_rel(d.android_rel, d.android_relsz, unpacked)) {
            err = name + ": bad packed relocations";
            ok = false;
        } else {
            ok = apply_rel(m, d, unpacked.data(), (u32)unpacked.size(), err);
        }
    }
    if (ok && d.rel) ok = apply_rel(m, d, d.rel, d.relsz / sizeof(Elf32_Rel), err);
    if (ok && d.jmprel) ok = apply_rel(m, d, d.jmprel, d.jmprelsz / sizeof(Elf32_Rel), err);
    if (!ok) {
        g_modules.erase(std::find(g_modules.begin(), g_modules.end(), m));
        page_free(m->base, m->size);
        delete m;
        return nullptr;
    }
    mem_bump_code_epoch();

    m->init_func = d.init ? m->bias + d.init : 0;
    m->init_array = d.init_array ? m->bias + d.init_array : 0;
    m->init_array_count = d.init_arraysz / 4;
    return m;
}

void run_constructors(Module* m) {
    if (m->constructed) return;
    m->constructed = true;
    for (auto& dep : m->needed)
        if (Module* dm = loader_find(dep.substr(dep.rfind('/') == std::string::npos ? 0 : dep.rfind('/') + 1))) run_constructors(dm);
    Cpu& c = Cpu::current();
    if (m->init_func) c.call(m->init_func, {});
    for (u32 i = 0; i < m->init_array_count; i++) {
        u32 fn = g2h<u32>(m->init_array)[i];
        if (fn && fn != 0xFFFFFFFF) c.call(fn, {});
    }
}

}  // namespace

void loader_add_provider(LibProvider p) {
    std::lock_guard<std::recursive_mutex> lk(g_load_mu);
    g_providers.push_back(std::move(p));
}

bool loader_is_system_lib(const std::string& name) {
    for (const char* s : kSystemLibs)
        if (name == s) return true;
    return false;
}

Module* loader_load(const std::string& path, std::string* error) {
    thunks_init();
    std::lock_guard<std::recursive_mutex> lk(g_load_mu);
    std::string err;
    Module* m = load_locked(base_name(path), err);
    if (!m) {
        LOGE("load failed: %s", err.c_str());
        if (error) *error = err;
        return nullptr;
    }
    run_constructors(m);
    return m;
}

Module* loader_find(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lk(g_load_mu);
    for (Module* m : g_modules)
        if (m->name == name) return m;
    return nullptr;
}

Module* loader_module_for(u32 addr) {
    std::lock_guard<std::recursive_mutex> lk(g_load_mu);
    for (Module* m : g_modules)
        if (addr >= m->base && addr < m->base + m->size) return m;
    return nullptr;
}

std::vector<Module*> loader_modules() {
    std::lock_guard<std::recursive_mutex> lk(g_load_mu);
    return g_modules;
}

u32 loader_lookup(const std::string& sym) {
    std::lock_guard<std::recursive_mutex> lk(g_load_mu);
    for (Module* m : g_modules)
        if (u32 a = m->find(sym)) return a;
    return 0;
}

std::string describe_address(u32 addr) {
    if (const char* t = trap_name_at(addr)) return strfmt("0x%08x <host:%s>", addr, t);
    // Also used from the crash handler: never wait for a lock another thread may hold forever.
    std::unique_lock<std::recursive_mutex> lk(g_load_mu, std::try_to_lock);
    if (!lk.owns_lock()) return strfmt("0x%08x", addr);
    Module* m = loader_module_for(addr);
    if (!m) return strfmt("0x%08x <unknown>", addr);
    std::string best;
    u32 best_addr = 0;
    for (auto& e : m->exports) {
        u32 a = e.second & ~1u;
        if (a <= addr && a > best_addr) {
            best_addr = a;
            best = e.first;
        }
    }
    if (best.empty()) return strfmt("0x%08x %s+0x%x", addr, m->name.c_str(), addr - m->bias);
    return strfmt("0x%08x %s+0x%x (%s+0x%x)", addr, m->name.c_str(), addr - m->bias, best.c_str(), addr - best_addr);
}

ElfInfo elf_inspect(const u8* data, size_t size) {
    ElfInfo info;
    if (!check_header(data, size, info.error, &info.machine)) {
        info.valid = size >= SELFMAG && memcmp(data, ELFMAG, SELFMAG) == 0;
        return info;
    }
    info.valid = true;
    info.arm32 = true;
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(data);
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(data + eh->e_phoff);
    u32 hi = 0, dyn_vaddr = 0, dyn_size = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            if ((u64)ph[i].p_offset + ph[i].p_filesz > size) {
                info.error = "corrupt segment";
                return info;
            }
            hi = std::max(hi, ph[i].p_vaddr + ph[i].p_memsz);
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dyn_vaddr = ph[i].p_vaddr;
            dyn_size = ph[i].p_memsz;
        }
    }
    if (hi == 0 || hi > (512u << 20)) {
        info.error = "unreasonable image size";
        return info;
    }
    std::vector<u8> image(hi);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_filesz <= ph[i].p_memsz)
            memcpy(image.data() + ph[i].p_vaddr, data + ph[i].p_offset, ph[i].p_filesz);
    Dyn d;
    d.origin = image.data();
    d.lo = 0;
    d.hi = hi;
    if (!dyn_size || !parse_dynamic(d, dyn_vaddr, dyn_size, info.error)) {
        if (info.error.empty()) info.error = "no dynamic section";
        return info;
    }
    for (u32 off : d.needed_off) info.needed.push_back(d.str(off));
    for (u32 i = 1; i < d.nsyms; i++) {
        const Elf32_Sym& s = d.symtab[i];
        const char* n = d.str(s.st_name);
        if (!*n) continue;
        if (s.st_shndx == SHN_UNDEF) {
            if (ELF32_ST_BIND(s.st_info) != STB_WEAK) info.imports.push_back(n);
        } else if (ELF32_ST_BIND(s.st_info) == STB_GLOBAL || ELF32_ST_BIND(s.st_info) == STB_WEAK) {
            info.exports.push_back(n);
        }
    }
    return info;
}

}  // namespace a64b
