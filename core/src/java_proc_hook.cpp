// SPDX-License-Identifier: GPL-2.0-or-later
//
// /proc files as Java code of a bridged app sees them.
//
// Some apps check the CPU from Java, not only from native code. The Java player of Unity 3 reads
// /proc/cpuinfo with a FileReader and needs "vfp" on the Features line and a CPU architecture of 6
// or more; otherwise it loads its "compatible mode" and turns audio off (Samurai vs Zombies:
// "Application was unable to load optimized libraries"). An arm64 kernel lists "fp asimd ...".
//
// Java opens files through the open() of libjavacore (libcore.io.Linux.open on Android) or of
// libopenjdk / libjava (FileInputStream on a desktop JVM). Their import of open() is redirected here
// (the GOT entry is rewritten), and /proc files that guest_proc_open() knows get the guest view.
// The patched app loads the runtime from a content provider (BridgeStarter), so this is in place
// before the app's own classes run.

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <mutex>

#include "common.h"
#include "thunk.h"

namespace a64b {
namespace {

using OpenFn = int (*)(const char*, int, ...);
using Open2Fn = int (*)(const char*, int);
OpenFn g_open, g_open64;
Open2Fn g_open_2, g_open64_2;

int mode_arg(int flags, va_list ap) {
#ifdef O_TMPFILE
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) return va_arg(ap, int);
#else
    if (flags & O_CREAT) return va_arg(ap, int);
#endif
    return 0;
}

int hook_open(const char* path, int flags, ...) {
    int v = guest_proc_open(path);
    if (v != -2) return v;
    va_list ap;
    va_start(ap, flags);
    int mode = mode_arg(flags, ap);
    va_end(ap);
    return g_open(path, flags, mode);
}

int hook_open64(const char* path, int flags, ...) {
    int v = guest_proc_open(path);
    if (v != -2) return v;
    va_list ap;
    va_start(ap, flags);
    int mode = mode_arg(flags, ap);
    va_end(ap);
    return g_open64(path, flags, mode);
}

int hook_open_2(const char* path, int flags) {
    int v = guest_proc_open(path);
    return v != -2 ? v : g_open_2(path, flags);
}

int hook_open64_2(const char* path, int flags) {
    int v = guest_proc_open(path);
    return v != -2 ? v : g_open64_2(path, flags);
}

struct Hook {
    const char* name;
    void* fn;
};

const Hook kHooks[] = {
    {"open", (void*)hook_open},
    {"open64", (void*)hook_open64},
    {"__open_2", (void*)hook_open_2},
    {"__open64_2", (void*)hook_open64_2},
};

const char* const kJavaLibs[] = {"libjavacore.so", "libopenjdk.so", "libjava.so"};

bool is_java_lib(const char* path) {
    if (!path) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (const char* l : kJavaLibs)
        if (strcmp(base, l) == 0) return true;
    return false;
}

// Rewrites the GOT entries of one library for the functions in kHooks.
int patch_library(const dl_phdr_info* info) {
    const ElfW(Addr) base = info->dlpi_addr;
    const ElfW(Dyn)* dyn = nullptr;
    ElfW(Addr) relro_start = 0, relro_end = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_DYNAMIC) dyn = reinterpret_cast<const ElfW(Dyn)*>(base + ph.p_vaddr);
        if (ph.p_type == PT_GNU_RELRO) {
            relro_start = base + ph.p_vaddr;
            relro_end = relro_start + ph.p_memsz;
        }
    }
    if (!dyn) return 0;
    // glibc stores absolute addresses in the dynamic section, bionic leaves them relative.
    auto ptr = [base](ElfW(Addr) v) { return v < base ? v + base : v; };
    const char* strtab = nullptr;
    const ElfW(Sym)* symtab = nullptr;
    ElfW(Addr) jmprel = 0, rela = 0;
    size_t jmprel_size = 0, rela_size = 0;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB: strtab = reinterpret_cast<const char*>(ptr(d->d_un.d_ptr)); break;
            case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym)*>(ptr(d->d_un.d_ptr)); break;
            case DT_JMPREL: jmprel = ptr(d->d_un.d_ptr); break;
            case DT_PLTRELSZ: jmprel_size = d->d_un.d_val; break;
            case DT_RELA: rela = ptr(d->d_un.d_ptr); break;
            case DT_RELASZ: rela_size = d->d_un.d_val; break;
        }
    }
    if (!strtab || !symtab) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    int patched = 0;
    for (auto [start, size] : {std::pair{jmprel, jmprel_size}, std::pair{rela, rela_size}}) {
        if (!start) continue;
        const auto* r = reinterpret_cast<const ElfW(Rela)*>(start);
        for (size_t i = 0; i < size / sizeof(ElfW(Rela)); i++) {
            const size_t sym = ELF64_R_SYM(r[i].r_info);
            if (!sym) continue;
            const char* name = strtab + symtab[sym].st_name;
            for (const Hook& h : kHooks) {
                if (strcmp(name, h.name) != 0) continue;
                const ElfW(Addr) slot = base + r[i].r_offset;
                void* page_start = reinterpret_cast<void*>(slot & ~(ElfW(Addr))(page - 1));
                const bool relro = slot >= relro_start && slot < relro_end;
                if (relro && mprotect(page_start, (size_t)page, PROT_READ | PROT_WRITE) != 0) break;
                *reinterpret_cast<void**>(slot) = h.fn;
                if (relro) mprotect(page_start, (size_t)page, PROT_READ);
                patched++;
            }
        }
    }
    return patched;
}

}  // namespace

void java_proc_hook_install() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_open = reinterpret_cast<OpenFn>(dlsym(RTLD_DEFAULT, "open"));
        g_open64 = reinterpret_cast<OpenFn>(dlsym(RTLD_DEFAULT, "open64"));
        g_open_2 = reinterpret_cast<Open2Fn>(dlsym(RTLD_DEFAULT, "__open_2"));
        g_open64_2 = reinterpret_cast<Open2Fn>(dlsym(RTLD_DEFAULT, "__open64_2"));
        if (!g_open64) g_open64 = g_open;
        if (!g_open64_2) g_open64_2 = g_open_2;
        if (!g_open) {
            LOGW("Java /proc view: open() not found");
            return;
        }
        // Make sure the Java libraries are loaded, then patch them.
        for (const char* l : kJavaLibs) dlopen(l, RTLD_NOW | RTLD_NOLOAD);
        int total = 0;
        dl_iterate_phdr(
            [](dl_phdr_info* info, size_t, void* data) -> int {
                if (is_java_lib(info->dlpi_name)) *static_cast<int*>(data) += patch_library(info);
                return 0;
            },
            &total);
        LOGI("Java /proc view: %d imports redirected (Java code sees the /proc/cpuinfo of a 32-bit ARM CPU)", total);
    });
}

}  // namespace a64b
