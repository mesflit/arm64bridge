// Host implementations of the bionic libc functions imported by guest libraries.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <mutex>
#include <unordered_map>

#include "guest_types.h"
#include "syscalls.h"
#include "thunk.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace a64b {

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()
#define STR() g2h<char>(a.next32())

namespace {

// ---- string helpers that hand host-owned strings to the guest ----
std::mutex g_str_mu;
u32 intern_guest_string(const char* s) {
    if (!s) return 0;
    static auto* cache = new std::unordered_map<std::string, u32>();
    std::lock_guard<std::mutex> lk(g_str_mu);
    auto it = cache->find(s);
    if (it != cache->end()) return it->second;
    u32 g = g_strdup(s);
    (*cache)[s] = g;
    return g;
}

// ---- in-place heap sort calling a guest comparator ----
void swap_bytes(u8* a, u8* b, size_t n) {
    while (n--) {
        u8 t = *a;
        *a++ = *b;
        *b++ = t;
    }
}
void guest_qsort(Cpu& c, u32 base, u32 n, u32 size, u32 cmp) {
    if (n < 2 || !size) return;
    u8* b = g2h<u8>(base);
    auto less = [&](u32 i, u32 j) { return (i32)c.call(cmp, {base + i * size, base + j * size}) < 0; };
    auto sift = [&](u32 start, u32 end) {
        u32 root = start;
        while (root * 2 + 1 <= end) {
            u32 child = root * 2 + 1, sw = root;
            if (less(sw, child)) sw = child;
            if (child + 1 <= end && less(sw, child + 1)) sw = child + 1;
            if (sw == root) return;
            swap_bytes(b + root * size, b + sw * size, size);
            root = sw;
        }
    };
    for (u32 start = (n - 2) / 2 + 1; start-- > 0;) sift(start, n - 1);
    for (u32 end = n - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        sift(0, end - 1);
    }
}

int translate_open_flags(int gf) {
#if defined(__aarch64__) || defined(__arm__)
    return gf;
#else
    // ARM uses different values for a few flags than the generic ABI.
    int hf = gf & ~(040000 | 0100000 | 0200000 | 0400000);
    if (gf & 040000) hf |= O_DIRECTORY;
    if (gf & 0100000) hf |= O_NOFOLLOW;
    if (gf & 0400000) hf |= O_LARGEFILE;
    return hf;
#endif
}

struct GuestDir {
    DIR* dir;
    u32 entry;  // guest buffer for the returned dirent
};
std::mutex g_dir_mu;
std::unordered_map<u32, GuestDir>& dirs() {
    static auto* m = new std::unordered_map<u32, GuestDir>();
    return *m;
}
GuestDir* find_dir(u32 h) {
    std::lock_guard<std::mutex> lk(g_dir_mu);
    auto it = dirs().find(h);
    return it == dirs().end() ? nullptr : &it->second;
}

u32 g_tm_buf = 0, g_ctime_buf = 0;
u32 tm_buf() {
    if (!g_tm_buf) g_tm_buf = g_calloc(1, sizeof(GTm));
    return g_tm_buf;
}

void do_mmap(Cpu& c, i64 offset_bytes, u32 addr, u32 len, int prot, int flags, int fd) {
    (void)addr;
    if (len == 0) {
        errno = EINVAL;
        c.set_ret32(0xFFFFFFFF);
        return;
    }
    u32 res;
    if (flags & MAP_ANONYMOUS) {
        res = page_alloc(len, (prot & PROT_EXEC) != 0);
    } else {
        int hflags = (flags & MAP_SHARED) ? MAP_SHARED : MAP_PRIVATE;
        res = page_map_file(len, prot, hflags, fd, offset_bytes);
    }
    if (!res) {
        errno = ENOMEM;
        res = 0xFFFFFFFF;
    }
    c.set_ret32(res);
}

// The in-game menu can relax the game's own frame limiter: sleeps of the thread that presents
// frames are shortened or skipped.
int limiter_mode_for_this_thread() {
    int mode = g_frame_limiter.load(std::memory_order_relaxed);
    // A known clock-based limiter is handled by changing its frame time (frame_cap.cpp).
    if (!mode || frame_cap_known()) return 0;
    static thread_local int tid = (int)syscall(SYS_gettid);
    return tid == g_render_tid.load(std::memory_order_relaxed) ? mode : 0;
}

template <class F>
void with_strto_end(Cpu& c, F f) {
    ArgReader a(c);
    const char* s = STR();
    u32 endp = A32();
    char* end = nullptr;
    f(a, s, &end);
    if (endp) *g2h<u32>(endp) = h2g(end);
}

}  // namespace

u32 guest_intern_string(const char* s) { return intern_guest_string(s); }

void register_libc_thunks() {
    // ------------------------------------------------------------- memory
    reg("malloc", +[](u32 n) -> u32 { return g_malloc(n); });
    reg("calloc", +[](u32 n, u32 s) -> u32 { return g_calloc(n, s); });
    reg("realloc", +[](u32 p, u32 n) -> u32 { return g_realloc(p, n); });
    reg("free", +[](u32 p) { g_free(p); });
    reg("memalign", +[](u32 al, u32 n) -> u32 { return g_memalign(al, n); });
    reg("aligned_alloc", +[](u32 al, u32 n) -> u32 { return g_memalign(al, n); });
    reg("valloc", +[](u32 n) -> u32 { return g_memalign(4096, n); });
    reg("posix_memalign", +[](u32* out, u32 al, u32 n) -> int {
        u32 p = g_memalign(al, n);
        if (!p) return ENOMEM;
        *out = p;
        return 0;
    });
    reg("malloc_usable_size", +[](u32 p) -> u32 { return g_usable_size(p); });
    reg("dlmalloc_usable_size", +[](u32 p) -> u32 { return g_usable_size(p); });
    // operator new / delete (system libstdc++)
    for (const char* n : {"_Znwj", "_Znaj", "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t"})
        reg(n, +[](u32 size) -> u32 {
            u32 p = g_malloc(size);
            if (!p) fatal("guest operator new: out of memory (%u bytes)", size);
            return p;
        });
    for (const char* n : {"_ZdlPv", "_ZdaPv", "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t", "_ZdlPvj", "_ZdaPvj"})
        reg(n, +[](u32 p) { g_free(p); });

    RAW(mmap) { ArgReader a(c); u32 addr = A32(), len = A32(); int prot = A32(), flags = A32(), fd = A32(); i64 off = (i32)A32(); do_mmap(c, off, addr, len, prot, flags, fd); });
    RAW(mmap64) { ArgReader a(c); u32 addr = A32(), len = A32(); int prot = A32(), flags = A32(), fd = A32(); i64 off = (i64)a.next64(); do_mmap(c, off, addr, len, prot, flags, fd); });
    RAW(__mmap2) { ArgReader a(c); u32 addr = A32(), len = A32(); int prot = A32(), flags = A32(), fd = A32(); i64 off = (i64)A32() * 4096; do_mmap(c, off, addr, len, prot, flags, fd); });
    reg("munmap", +[](u32 addr, u32 len) -> int {
        if (page_free(addr, len)) return 0;
        errno = EINVAL;
        return -1;
    });
    reg("mprotect", +[](u32 addr, u32 len, int prot) -> int {
        if (prot & PROT_EXEC) {
            page_mark_exec(addr, len);
            mem_bump_code_epoch();
        }
        return 0;
    });
    reg("madvise", +[](u32, u32, int) -> int { return 0; });
    reg("mlock", +[](u32, u32) -> int { return 0; });
    reg("munlock", +[](u32, u32) -> int { return 0; });
    reg("msync", +[](void* p, size_t n, int f) -> int { return msync(p, n, f); });
    reg("cacheflush", +[](u32, u32, int) -> int { mem_bump_code_epoch(); return 0; });
    reg("__clear_cache", +[](u32, u32) { mem_bump_code_epoch(); });

    // ------------------------------------------------------------- string
    reg("memcpy", +[](void* d, const void* s, size_t n) -> void* { return memcpy(d, s, n); });
    reg("memmove", +[](void* d, const void* s, size_t n) -> void* { return memmove(d, s, n); });
    reg("memset", +[](void* d, int v, size_t n) -> void* { return memset(d, v, n); });
    reg("memcmp", +[](const void* x, const void* y, size_t n) -> int { return memcmp(x, y, n); });
    reg("bcmp", +[](const void* x, const void* y, size_t n) -> int { return memcmp(x, y, n); });
    reg("bcopy", +[](const void* s, void* d, size_t n) { memmove(d, s, n); });
    reg("bzero", +[](void* d, size_t n) { memset(d, 0, n); });
    reg("memchr", +[](const void* s, int ch, size_t n) -> void* { return (void*)memchr(s, ch, n); });
    reg("memrchr", +[](const void* s, int ch, size_t n) -> void* { return (void*)memrchr(s, ch, n); });
    reg("memmem", +[](const void* h, size_t hl, const void* n, size_t nl) -> void* { return (void*)memmem(h, hl, n, nl); });
    reg("memccpy", +[](void* d, const void* s, int ch, size_t n) -> void* { return memccpy(d, s, ch, n); });
    reg("__memcpy_chk", +[](void* d, const void* s, size_t n, size_t) -> void* { return memcpy(d, s, n); });
    reg("__memmove_chk", +[](void* d, const void* s, size_t n, size_t) -> void* { return memmove(d, s, n); });
    reg("__memset_chk", +[](void* d, int v, size_t n, size_t) -> void* { return memset(d, v, n); });
    reg("strlen", +[](const char* s) -> size_t { return strlen(s); });
    reg("__strlen_chk", +[](const char* s, size_t) -> size_t { return strlen(s); });
    reg("strnlen", +[](const char* s, size_t n) -> size_t { return strnlen(s, n); });
    reg("strcpy", +[](char* d, const char* s) -> char* { return strcpy(d, s); });
    reg("__strcpy_chk", +[](char* d, const char* s, size_t) -> char* { return strcpy(d, s); });
    reg("stpcpy", +[](char* d, const char* s) -> char* { return stpcpy(d, s); });
    reg("strncpy", +[](char* d, const char* s, size_t n) -> char* { return strncpy(d, s, n); });
    reg("__strncpy_chk", +[](char* d, const char* s, size_t n, size_t) -> char* { return strncpy(d, s, n); });
    reg("strcat", +[](char* d, const char* s) -> char* { return strcat(d, s); });
    reg("__strcat_chk", +[](char* d, const char* s, size_t) -> char* { return strcat(d, s); });
    reg("strncat", +[](char* d, const char* s, size_t n) -> char* { return strncat(d, s, n); });
    reg("__strncat_chk", +[](char* d, const char* s, size_t n, size_t) -> char* { return strncat(d, s, n); });
    reg("strlcpy", +[](char* d, const char* s, size_t n) -> size_t {
        size_t l = strlen(s);
        if (n) {
            size_t k = l < n - 1 ? l : n - 1;
            memcpy(d, s, k);
            d[k] = 0;
        }
        return l;
    });
    reg("strlcat", +[](char* d, const char* s, size_t n) -> size_t {
        size_t dl = strnlen(d, n), sl = strlen(s);
        if (dl == n) return n + sl;
        size_t k = sl < n - dl - 1 ? sl : n - dl - 1;
        memcpy(d + dl, s, k);
        d[dl + k] = 0;
        return dl + sl;
    });
    reg("strcmp", +[](const char* x, const char* y) -> int { return strcmp(x, y); });
    reg("strncmp", +[](const char* x, const char* y, size_t n) -> int { return strncmp(x, y, n); });
    reg("strcasecmp", +[](const char* x, const char* y) -> int { return strcasecmp(x, y); });
    reg("strncasecmp", +[](const char* x, const char* y, size_t n) -> int { return strncasecmp(x, y, n); });
    reg("strcoll", +[](const char* x, const char* y) -> int { return strcmp(x, y); });
    reg("strxfrm", +[](char* d, const char* s, size_t n) -> size_t { return strxfrm(d, s, n); });
    reg("strchr", +[](const char* s, int ch) -> char* { return (char*)strchr(s, ch); });
    reg("index", +[](const char* s, int ch) -> char* { return (char*)strchr(s, ch); });
    reg("__strchr_chk", +[](const char* s, int ch, size_t) -> char* { return (char*)strchr(s, ch); });
    reg("strrchr", +[](const char* s, int ch) -> char* { return (char*)strrchr(s, ch); });
    reg("rindex", +[](const char* s, int ch) -> char* { return (char*)strrchr(s, ch); });
    reg("__strrchr_chk", +[](const char* s, int ch, size_t) -> char* { return (char*)strrchr(s, ch); });
    reg("strstr", +[](const char* h, const char* n) -> char* { return (char*)strstr(h, n); });
    reg("strcasestr", +[](const char* h, const char* n) -> char* { return (char*)strcasestr(h, n); });
    reg("strpbrk", +[](const char* s, const char* acc) -> char* { return (char*)strpbrk(s, acc); });
    reg("strspn", +[](const char* s, const char* acc) -> size_t { return strspn(s, acc); });
    reg("strcspn", +[](const char* s, const char* rej) -> size_t { return strcspn(s, rej); });
    reg("strtok", +[](char* s, const char* d) -> char* { return strtok(s, d); });
    RAW(strtok_r) {
        ArgReader a(c);
        char* s = STR();
        const char* d = STR();
        u32* save = g2h<u32>(A32());
        char* sp = g2h<char>(*save);
        char* r = strtok_r(s, d, &sp);
        *save = h2g(sp);
        c.set_ret32(h2g(r));
    });
    RAW(strsep) {
        ArgReader a(c);
        u32* sp = g2h<u32>(A32());
        const char* d = STR();
        char* s = g2h<char>(*sp);
        char* r = *sp ? strsep(&s, d) : nullptr;
        if (*sp) *sp = h2g(s);
        c.set_ret32(h2g(r));
    });
    reg("strdup", +[](const char* s) -> u32 { return g_strdup(s); });
    reg("strndup", +[](const char* s, size_t n) -> u32 {
        size_t l = strnlen(s, n);
        u32 p = g_malloc((u32)l + 1);
        if (p) {
            memcpy(g2h(p), s, l);
            g2h<char>(p)[l] = 0;
        }
        return p;
    });
    reg("strerror", +[](int e) -> u32 { return intern_guest_string(strerror(e)); });
    reg("strerror_r", +[](int e, char* buf, size_t n) -> int {
        snprintf(buf, n, "%s", strerror(e));
        return 0;
    });
    reg("strsignal", +[](int s) -> u32 { return intern_guest_string(strsignal(s)); });

    // number parsing (char** end pointers are 4 bytes wide in the guest)
    RAW(strtol) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) { c.set_ret32((u32)strtol(s, e, (int)A32()) ); }); });
    RAW(strtoul) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) {
        unsigned long long v = strtoull(s, e, (int)A32());
        c.set_ret32(v > 0xFFFFFFFFull && (long long)v > 0 ? 0xFFFFFFFFu : (u32)v);
    }); });
    RAW(strtoll) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) { c.set_ret64((u64)strtoll(s, e, (int)A32())); }); });
    RAW(strtoull) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) { c.set_ret64(strtoull(s, e, (int)A32())); }); });
    RAW(strtoimax) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) { c.set_ret64((u64)strtoll(s, e, (int)A32())); }); });
    RAW(strtoumax) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) { c.set_ret64(strtoull(s, e, (int)A32())); }); });
    RAW(strtoq) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) { c.set_ret64((u64)strtoll(s, e, (int)A32())); }); });
    RAW(strtouq) { with_strto_end(c, [&](ArgReader& a, const char* s, char** e) { c.set_ret64(strtoull(s, e, (int)A32())); }); });
    RAW(strtod) { with_strto_end(c, [&](ArgReader&, const char* s, char** e) { double d = strtod(s, e); u64 v; memcpy(&v, &d, 8); c.set_ret64(v); }); });
    RAW(strtold) { with_strto_end(c, [&](ArgReader&, const char* s, char** e) { double d = strtod(s, e); u64 v; memcpy(&v, &d, 8); c.set_ret64(v); }); });
    RAW(strtof) { with_strto_end(c, [&](ArgReader&, const char* s, char** e) { float f = strtof(s, e); u32 v; memcpy(&v, &f, 4); c.set_ret32(v); }); });
    reg("atoi", +[](const char* s) -> int { return atoi(s); });
    reg("atol", +[](const char* s) -> int { return atoi(s); });
    reg("atoll", +[](const char* s) -> long long { return atoll(s); });
    reg("atof", +[](const char* s) -> double { return atof(s); });

    // ------------------------------------------------------------- ctype / wchar
#define CT(name) reg(#name, +[](int ch) -> int { return name(ch); });
    CT(isalpha) CT(isdigit) CT(isalnum) CT(isspace) CT(isupper) CT(islower) CT(isxdigit) CT(ispunct) CT(isprint)
    CT(isgraph) CT(iscntrl) CT(isblank) CT(tolower) CT(toupper) CT(isascii) CT(toascii)
#undef CT
#define WCT(name) reg(#name, +[](wint_t ch) -> int { return (int)name(ch); });
    WCT(iswalpha) WCT(iswdigit) WCT(iswalnum) WCT(iswspace) WCT(iswupper) WCT(iswlower) WCT(iswxdigit) WCT(iswpunct)
    WCT(iswprint) WCT(iswcntrl) WCT(towlower) WCT(towupper)
#undef WCT
    reg("wcslen", +[](const wchar_t* s) -> size_t { return wcslen(s); });
    reg("wcscpy", +[](wchar_t* d, const wchar_t* s) -> wchar_t* { return wcscpy(d, s); });
    reg("wcsncpy", +[](wchar_t* d, const wchar_t* s, size_t n) -> wchar_t* { return wcsncpy(d, s, n); });
    reg("wcscat", +[](wchar_t* d, const wchar_t* s) -> wchar_t* { return wcscat(d, s); });
    reg("wcscmp", +[](const wchar_t* x, const wchar_t* y) -> int { return wcscmp(x, y); });
    reg("wcsncmp", +[](const wchar_t* x, const wchar_t* y, size_t n) -> int { return wcsncmp(x, y, n); });
    reg("wcscoll", +[](const wchar_t* x, const wchar_t* y) -> int { return wcscmp(x, y); });
    reg("wcschr", +[](const wchar_t* s, wchar_t ch) -> wchar_t* { return (wchar_t*)wcschr(s, ch); });
    reg("wcsrchr", +[](const wchar_t* s, wchar_t ch) -> wchar_t* { return (wchar_t*)wcsrchr(s, ch); });
    reg("wcsstr", +[](const wchar_t* h, const wchar_t* n) -> wchar_t* { return (wchar_t*)wcsstr(h, n); });
    reg("wmemcpy", +[](wchar_t* d, const wchar_t* s, size_t n) -> wchar_t* { return wmemcpy(d, s, n); });
    reg("wmemmove", +[](wchar_t* d, const wchar_t* s, size_t n) -> wchar_t* { return wmemmove(d, s, n); });
    reg("wmemset", +[](wchar_t* d, wchar_t ch, size_t n) -> wchar_t* { return wmemset(d, ch, n); });
    reg("wmemcmp", +[](const wchar_t* x, const wchar_t* y, size_t n) -> int { return wmemcmp(x, y, n); });
    reg("wmemchr", +[](const wchar_t* s, wchar_t ch, size_t n) -> wchar_t* { return (wchar_t*)wmemchr(s, ch, n); });
    reg("mbstowcs", +[](wchar_t* d, const char* s, size_t n) -> size_t { return mbstowcs(d, s, n); });
    reg("wcstombs", +[](char* d, const wchar_t* s, size_t n) -> size_t { return wcstombs(d, s, n); });
    reg("mbtowc", +[](wchar_t* d, const char* s, size_t n) -> int { return mbtowc(d, s, n); });
    reg("wctomb", +[](char* d, wchar_t ch) -> int { return wctomb(d, ch); });
    reg("mblen", +[](const char* s, size_t n) -> int { return mblen(s, n); });
    reg("btowc", +[](int ch) -> wint_t { return btowc(ch); });
    reg("wctob", +[](wint_t ch) -> int { return wctob(ch); });
    reg("mbrtowc", +[](wchar_t* d, const char* s, size_t n, void*) -> size_t { return mbrtowc(d, s, n, nullptr); });
    reg("wcrtomb", +[](char* d, wchar_t ch, void*) -> size_t { return wcrtomb(d, ch, nullptr); });
    reg("mbrlen", +[](const char* s, size_t n, void*) -> size_t { return mbrlen(s, n, nullptr); });
    reg("mbsinit", +[](void*) -> int { return 1; });
    reg("__ctype_get_mb_cur_max", +[]() -> size_t { return MB_CUR_MAX; });
    reg("setlocale", +[](int, const char*) -> u32 { return intern_guest_string("C"); });
    reg("newlocale", +[](int, const char*, u32) -> u32 { return intern_guest_string("<locale>"); });
    reg("duplocale", +[](u32 l) -> u32 { return l; });
    reg("freelocale", +[](u32) {});
    reg("uselocale", +[](u32) -> u32 { return intern_guest_string("<locale>"); });
    RAW(localeconv) {
        static u32 g = 0;
        if (!g) {
            // struct lconv: 10 char* followed by 14 chars.
            g = g_calloc(1, 64);
            u32 dot = intern_guest_string("."), empty = intern_guest_string("");
            for (int i = 0; i < 10; i++) g2h<u32>(g)[i] = empty;
            g2h<u32>(g)[0] = dot;
            memset(g2h<u8>(g) + 40, 0x7f, 14);
        }
        c.set_ret32(g);
    });

    // ------------------------------------------------------------- stdlib
    RAW(qsort) { ArgReader a(c); u32 b = A32(), n = A32(), sz = A32(), cmp = A32(); guest_qsort(c, b, n, sz, cmp); });
    RAW(bsearch) {
        ArgReader a(c);
        u32 key = A32(), base = A32(), n = A32(), size = A32(), cmp = A32();
        u32 lo = 0, hi = n, found = 0;
        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2, elem = base + mid * size;
            i32 r = (i32)c.call(cmp, {key, elem});
            if (r == 0) {
                found = elem;
                break;
            }
            if (r < 0) hi = mid; else lo = mid + 1;
        }
        c.set_ret32(found);
    });
    reg("abs", +[](int v) -> int { return abs(v); });
    reg("labs", +[](int v) -> int { return abs(v); });
    reg("llabs", +[](long long v) -> long long { return llabs(v); });
    reg("rand", +[]() -> int { return rand() & 0x7fffffff; });
    reg("srand", +[](unsigned s) { srand(s); });
    reg("random", +[]() -> int { return (int)(random() & 0x7fffffff); });
    reg("srandom", +[](unsigned s) { srandom(s); });
    reg("rand_r", +[](unsigned* s) -> int { return rand_r(s); });
    reg("lrand48", +[]() -> int { return (int)lrand48(); });
    reg("mrand48", +[]() -> int { return (int)mrand48(); });
    reg("drand48", +[]() -> double { return drand48(); });
    reg("srand48", +[](int s) { srand48(s); });
    reg("arc4random", +[]() -> u32 { return (u32)random() ^ ((u32)random() << 16); });
    reg("arc4random_uniform", +[](u32 n) -> u32 { return n ? (u32)random() % n : 0; });
    reg("getenv", +[](const char* n) -> u32 { return intern_guest_string(getenv(n)); });
    reg("setenv", +[](const char* n, const char* v, int o) -> int { return setenv(n, v, o); });
    reg("unsetenv", +[](const char* n) -> int { return unsetenv(n); });
    reg("putenv", +[](char*) -> int { return 0; });
    reg("abort", +[]() { fatal("guest called abort()"); });
    reg("exit", +[](int code) { LOGW("guest called exit(%d)", code); exit(code); });
    reg("_exit", +[](int code) { _exit(code); });
    reg("_Exit", +[](int code) { _exit(code); });
    reg("atexit", +[](u32) -> int { return 0; });
    reg("__cxa_atexit", +[](u32, u32, u32) -> int { return 0; });
    reg("__aeabi_atexit", +[](u32, u32, u32) -> int { return 0; });
    reg("__cxa_finalize", +[](u32) {});
    reg("__cxa_thread_atexit_impl", +[](u32, u32, u32) -> int { return 0; });
    reg("__register_atfork", +[](u32, u32, u32, u32) -> int { return 0; });
    reg("pthread_atfork", +[](u32, u32, u32) -> int { return 0; });
    reg("__stack_chk_fail", +[]() { fatal("guest stack corruption detected (__stack_chk_fail)"); });
    reg("__assert2", +[](const char* file, int line, const char* fn, const char* expr) {
        fatal("guest assertion failed: %s:%d %s: %s", file, line, fn ? fn : "?", expr);
    });
    reg("__assert", +[](const char* file, int line, const char* expr) { fatal("guest assertion failed: %s:%d: %s", file, line, expr); });
    reg("__cxa_pure_virtual", +[]() { fatal("guest: pure virtual function called"); });
    reg("__errno", +[]() -> u32 { return Cpu::current().errno_addr; });
    reg("realpath", +[](const char* path, char* out) -> u32 {
        char buf[PATH_MAX];
        if (!realpath(path, buf)) return 0;
        if (out) {
            strcpy(out, buf);
            return h2g(out);
        }
        return g_strdup(buf);
    });
    reg("mkstemp", +[](char* t) -> int { return mkstemp(t); });
    reg("mktemp", +[](char* t) -> char* { int fd = mkstemp(t); if (fd >= 0) { close(fd); unlink(t); } return t; });
    reg("system", +[](const char*) -> int { return -1; });

    // ------------------------------------------------------------- time
    RAW(time) {
        ArgReader a(c);
        u32 out = A32();
        time_t t = time(nullptr);
        if (out) *g2h<i32>(out) = (i32)t;
        c.set_ret32((u32)t);
    });
    reg("gettimeofday", +[](GTimeval* tv, void*) -> int {
        probe_time_call(0);
        if (Cpu* c = Cpu::current_or_null()) frame_cap_on_clock(*c);
        timeval h;
        gettimeofday(&h, nullptr);
        if (tv) { tv->tv_sec = (i32)h.tv_sec; tv->tv_usec = (i32)h.tv_usec; }
        return 0;
    });
    reg("clock_gettime", +[](int clk, GTimespec* ts) -> int {
        probe_time_call(1);
        timespec h;
        int r = clock_gettime(clk, &h);
        if (r == 0 && ts) to_guest(ts, h);
        return r;
    });
    reg("clock_getres", +[](int clk, GTimespec* ts) -> int {
        timespec h;
        int r = clock_getres(clk, &h);
        if (r == 0 && ts) to_guest(ts, h);
        return r;
    });
    reg("clock", +[]() -> u32 { return (u32)clock(); });
    reg("nanosleep", +[](const GTimespec* req, GTimespec* rem) -> int {
        timespec h = from_guest(req), hr = {};
        probe_time_call(2, (u64)h.tv_sec * 1000000ull + (u64)h.tv_nsec / 1000);
        if (int mode = limiter_mode_for_this_thread()) {
            if (mode == 2) { if (rem) to_guest(rem, hr); return 0; }
            long long ns = (h.tv_sec * 1000000000ll + h.tv_nsec) / 2;
            h.tv_sec = ns / 1000000000ll;
            h.tv_nsec = ns % 1000000000ll;
        }
        int r = nanosleep(&h, &hr);
        if (rem) to_guest(rem, hr);
        return r;
    });
    reg("clock_nanosleep", +[](int clk, int flags, const GTimespec* req, GTimespec* rem) -> int {
        timespec h = from_guest(req), hr = {};
        int r = clock_nanosleep(clk, flags, &h, &hr);
        if (rem) to_guest(rem, hr);
        return r;
    });
    reg("usleep", +[](u32 us) -> int {
        probe_time_call(2, us);
        if (int mode = limiter_mode_for_this_thread()) {
            if (mode == 2) return 0;
            us /= 2;
        }
        return usleep(us);
    });
    reg("sleep", +[](u32 s) -> u32 { return sleep(s); });
    auto tm_conv = +[](const i32* t, GTm* out, int local) -> u32 {
        time_t tt = *t;
        struct tm h;
        if (local) localtime_r(&tt, &h); else gmtime_r(&tt, &h);
        u32 dst = out ? h2g(out) : tm_buf();
        to_guest(g2h<GTm>(dst), h, intern_guest_string(h.tm_zone ? h.tm_zone : "UTC"));
        return dst;
    };
    static auto s_tm_conv = tm_conv;
    reg("localtime", +[](const i32* t) -> u32 { return s_tm_conv(t, nullptr, 1); });
    reg("gmtime", +[](const i32* t) -> u32 { return s_tm_conv(t, nullptr, 0); });
    reg("localtime_r", +[](const i32* t, GTm* o) -> u32 { return s_tm_conv(t, o, 1); });
    reg("gmtime_r", +[](const i32* t, GTm* o) -> u32 { return s_tm_conv(t, o, 0); });
    reg("mktime", +[](GTm* g) -> i32 {
        struct tm h = from_guest(g);
        time_t r = mktime(&h);
        to_guest(g, h, g->tm_zone);
        return (i32)r;
    });
    reg("timegm", +[](GTm* g) -> i32 { struct tm h = from_guest(g); return (i32)timegm(&h); });
    reg("strftime", +[](char* buf, size_t n, const char* fmt, const GTm* g) -> size_t {
        struct tm h = from_guest(g);
        return strftime(buf, n, fmt, &h);
    });
    reg("asctime", +[](const GTm* g) -> u32 {
        struct tm h = from_guest(g);
        if (!g_ctime_buf) g_ctime_buf = g_calloc(1, 64);
        asctime_r(&h, g2h<char>(g_ctime_buf));
        return g_ctime_buf;
    });
    reg("ctime", +[](const i32* t) -> u32 {
        time_t tt = *t;
        if (!g_ctime_buf) g_ctime_buf = g_calloc(1, 64);
        ctime_r(&tt, g2h<char>(g_ctime_buf));
        return g_ctime_buf;
    });
    reg("difftime", +[](i32 x, i32 y) -> double { return (double)x - (double)y; });
    reg("tzset", +[]() { tzset(); });

    // ------------------------------------------------------------- files / unistd
    RAW(open) { ArgReader a(c); const char* p = STR(); int fl = A32(); int mode = A32(); c.set_ret32((u32)open(p, translate_open_flags(fl), mode)); });
    RAW(open64) { ArgReader a(c); const char* p = STR(); int fl = A32(); int mode = A32(); c.set_ret32((u32)open(p, translate_open_flags(fl), mode)); });
    RAW(__open_2) { ArgReader a(c); const char* p = STR(); int fl = A32(); c.set_ret32((u32)open(p, translate_open_flags(fl), 0)); });
    RAW(openat) { ArgReader a(c); int d = A32(); const char* p = STR(); int fl = A32(); int mode = A32(); c.set_ret32((u32)openat(d, p, translate_open_flags(fl), mode)); });
    reg("creat", +[](const char* p, int mode) -> int { return creat(p, mode); });
    reg("close", +[](int fd) -> int { return close(fd); });
    reg("read", +[](int fd, void* b, size_t n) -> ssize_t { return read(fd, b, n); });
    reg("__read_chk", +[](int fd, void* b, size_t n, size_t) -> ssize_t { return read(fd, b, n); });
    reg("write", +[](int fd, const void* b, size_t n) -> ssize_t { return write(fd, b, n); });
    reg("pread", +[](int fd, void* b, size_t n, i32 off) -> ssize_t { return pread(fd, b, n, off); });
    reg("pwrite", +[](int fd, const void* b, size_t n, i32 off) -> ssize_t { return pwrite(fd, b, n, off); });
    reg("pread64", +[](int fd, void* b, size_t n, long long off) -> ssize_t { return pread(fd, b, n, off); });
    reg("pwrite64", +[](int fd, const void* b, size_t n, long long off) -> ssize_t { return pwrite(fd, b, n, off); });
    reg("lseek", +[](int fd, i32 off, int wh) -> i32 { return (i32)lseek(fd, off, wh); });
    reg("lseek64", +[](int fd, long long off, int wh) -> long long { return lseek(fd, off, wh); });
    reg("fsync", +[](int fd) -> int { return fsync(fd); });
    reg("fdatasync", +[](int fd) -> int { return fdatasync(fd); });
    reg("ftruncate", +[](int fd, i32 len) -> int { return ftruncate(fd, len); });
    reg("ftruncate64", +[](int fd, long long len) -> int { return ftruncate(fd, len); });
    reg("truncate", +[](const char* p, i32 len) -> int { return truncate(p, len); });
    reg("access", +[](const char* p, int m) -> int { return access(p, m); });
    reg("unlink", +[](const char* p) -> int { return unlink(p); });
    reg("rmdir", +[](const char* p) -> int { return rmdir(p); });
    reg("mkdir", +[](const char* p, int m) -> int { return mkdir(p, m); });
    reg("chdir", +[](const char* p) -> int { return chdir(p); });
    reg("chmod", +[](const char* p, int m) -> int { return chmod(p, m); });
    reg("fchmod", +[](int fd, int m) -> int { return fchmod(fd, m); });
    reg("rename", +[](const char* x, const char* y) -> int { return rename(x, y); });
    reg("remove", +[](const char* p) -> int { return remove(p); });
    reg("link", +[](const char* x, const char* y) -> int { return link(x, y); });
    reg("symlink", +[](const char* x, const char* y) -> int { return symlink(x, y); });
    reg("readlink", +[](const char* p, char* b, size_t n) -> ssize_t { return readlink(p, b, n); });
    reg("getcwd", +[](char* b, size_t n) -> char* { return getcwd(b, n); });
    reg("dup", +[](int fd) -> int { return dup(fd); });
    reg("dup2", +[](int x, int y) -> int { return dup2(x, y); });
    reg("pipe", +[](int* fds) -> int { return pipe(fds); });
    reg("isatty", +[](int fd) -> int { return isatty(fd); });
    reg("umask", +[](int m) -> int { return umask(m); });
    reg("getpid", +[]() -> int { return getpid(); });
    reg("getppid", +[]() -> int { return getppid(); });
    reg("gettid", +[]() -> int { return (int)syscall(SYS_gettid); });
    reg("getuid", +[]() -> int { return getuid(); });
    reg("geteuid", +[]() -> int { return geteuid(); });
    reg("getgid", +[]() -> int { return getgid(); });
    reg("getegid", +[]() -> int { return getegid(); });
    reg("getpagesize", +[]() -> int { return 4096; });
    reg("sysconf", +[](int name) -> int {
        // bionic and the host C library use different _SC_ numbers; handle the common ones.
        switch (name) {
            case 0x27: case 0x28: return 4096;                                // _SC_PAGESIZE / _SC_PAGE_SIZE
            case 0x60: return (int)sysconf(_SC_NPROCESSORS_CONF);              // _SC_NPROCESSORS_CONF
            case 0x61: return (int)sysconf(_SC_NPROCESSORS_ONLN);              // _SC_NPROCESSORS_ONLN
            case 0x62: return (int)(sysconf(_SC_PHYS_PAGES) > 0x7fffffff ? 0x7fffffff : sysconf(_SC_PHYS_PAGES));
            case 0x06: return 100;                                             // _SC_CLK_TCK
            case 0x00: return 131072;                                          // _SC_ARG_MAX
            case 0x0a: return 1024;                                            // _SC_OPEN_MAX
            default: return -1;
        }
    });
    reg("sched_yield", +[]() -> int {
        probe_time_call(3);
        return sched_yield();
    });
    reg("sched_getcpu", +[]() -> int { return 0; });
    reg("sched_get_priority_max", +[](int p) -> int { return sched_get_priority_max(p); });
    reg("sched_get_priority_min", +[](int p) -> int { return sched_get_priority_min(p); });
    reg("sched_setscheduler", +[](int, int, void*) -> int { return 0; });
    reg("sched_setaffinity", +[](int, u32, void*) -> int { return 0; });
    reg("setpriority", +[](int which, int who, int prio) -> int {
        // Thread priorities matter on phones: the scheduler keeps busy, high-priority threads on fast cores.
        if (setpriority(which, who, prio) == 0) return 0;
        return 0;  // not permitted at this level: behave as before (succeed silently)
    });
    reg("getpriority", +[](int, int) -> int { return 0; });
    reg("nice", +[](int inc) -> int { errno = 0; int r = nice(inc); return (r == -1 && errno) ? 0 : r; });
    reg("prctl", +[](int, u32, u32, u32, u32) -> int { return 0; });
    reg("getrlimit", +[](int, u32* out) -> int { out[0] = out[1] = 0x7fffffff; return 0; });
    reg("setrlimit", +[](int, void*) -> int { return 0; });
    RAW(fcntl) { ArgReader a(c); int fd = A32(), cmd = A32(); u32 arg = A32(); c.set_ret32((u32)fcntl(fd, cmd, (long)arg)); });
    RAW(syscall) {
        ArgReader a(c);
        u32 nr = A32();
        u32 args[6];
        for (auto& x : args) x = A32();
        u32 r = guest_syscall_args(c, nr, args);
        if ((i32)r < 0 && (i32)r >= -4095) {
            errno = -(i32)r;
            r = (u32)-1;
        }
        c.set_ret32(r);
    });

#define STATFN(name, expr)                                               \
    reg(#name, +[](const char* p, GStat* out) -> int {                   \
        struct stat s;                                                   \
        int r = expr;                                                    \
        if (r == 0) to_guest(out, s);                                    \
        return r;                                                        \
    });
    STATFN(stat, stat(p, &s)) STATFN(lstat, lstat(p, &s)) STATFN(stat64, stat(p, &s)) STATFN(lstat64, lstat(p, &s))
#undef STATFN
    auto fstat_fn = +[](int fd, GStat* out) -> int {
        struct stat s;
        int r = fstat(fd, &s);
        if (r == 0) to_guest(out, s);
        return r;
    };
    reg("fstat", fstat_fn);
    reg("fstat64", fstat_fn);
    reg("fstatat", +[](int d, const char* p, GStat* out, int fl) -> int {
        struct stat s;
        int r = fstatat(d, p, &s, fl);
        if (r == 0) to_guest(out, s);
        return r;
    });

    // directories (struct dirent has the same layout in 32-bit bionic and on 64-bit hosts)
    reg("opendir", +[](const char* p) -> u32 {
        DIR* d = opendir(p);
        if (!d) return 0;
        u32 h = g_calloc(1, sizeof(struct dirent) + 16);
        std::lock_guard<std::mutex> lk(g_dir_mu);
        dirs()[h] = GuestDir{d, h};
        return h;
    });
    reg("readdir", +[](u32 h) -> u32 {
        GuestDir* gd = find_dir(h);
        if (!gd) return 0;
        struct dirent* e = readdir(gd->dir);
        if (!e) return 0;
        memcpy(g2h(gd->entry), e, sizeof(struct dirent));
        return gd->entry;
    });
    reg("readdir_r", +[](u32 h, struct dirent* entry, u32* result) -> int {
        GuestDir* gd = find_dir(h);
        struct dirent* e = gd ? readdir(gd->dir) : nullptr;
        if (!e) {
            *result = 0;
            return 0;
        }
        memcpy(entry, e, sizeof(struct dirent));
        *result = h2g(entry);
        return 0;
    });
    reg("rewinddir", +[](u32 h) { if (GuestDir* gd = find_dir(h)) rewinddir(gd->dir); });
    reg("dirfd", +[](u32 h) -> int { GuestDir* gd = find_dir(h); return gd ? dirfd(gd->dir) : -1; });
    reg("closedir", +[](u32 h) -> int {
        DIR* d = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_dir_mu);
            auto it = dirs().find(h);
            if (it == dirs().end()) return -1;
            d = it->second.dir;
            dirs().erase(it);
        }
        g_free(h);
        return closedir(d);
    });

    // ------------------------------------------------------------- sockets / polling
    reg("socket", +[](int d, int t, int p) -> int { return socket(d, t, p); });
    reg("connect", +[](int fd, const sockaddr* sa, socklen_t l) -> int { return connect(fd, sa, l); });
    reg("bind", +[](int fd, const sockaddr* sa, socklen_t l) -> int { return bind(fd, sa, l); });
    reg("listen", +[](int fd, int n) -> int { return listen(fd, n); });
    reg("accept", +[](int fd, sockaddr* sa, socklen_t* l) -> int { return accept(fd, sa, l); });
    reg("send", +[](int fd, const void* b, size_t n, int f) -> ssize_t { return send(fd, b, n, f); });
    reg("recv", +[](int fd, void* b, size_t n, int f) -> ssize_t { return recv(fd, b, n, f); });
    reg("sendto", +[](int fd, const void* b, size_t n, int f, const sockaddr* sa, socklen_t l) -> ssize_t { return sendto(fd, b, n, f, sa, l); });
    reg("recvfrom", +[](int fd, void* b, size_t n, int f, sockaddr* sa, socklen_t* l) -> ssize_t { return recvfrom(fd, b, n, f, sa, l); });
    reg("shutdown", +[](int fd, int how) -> int { return shutdown(fd, how); });
    reg("setsockopt", +[](int fd, int lvl, int opt, const void* v, socklen_t l) -> int { return setsockopt(fd, lvl, opt, v, l); });
    reg("getsockopt", +[](int fd, int lvl, int opt, void* v, socklen_t* l) -> int { return getsockopt(fd, lvl, opt, v, l); });
    reg("getsockname", +[](int fd, sockaddr* sa, socklen_t* l) -> int { return getsockname(fd, sa, l); });
    reg("getpeername", +[](int fd, sockaddr* sa, socklen_t* l) -> int { return getpeername(fd, sa, l); });
    reg("poll", +[](pollfd* fds, u32 n, int timeout) -> int { return poll(fds, n, timeout); });
    reg("select", +[](int n, fd_set* r, fd_set* w, fd_set* e, GTimeval* tv) -> int {
        timeval h, *hp = nullptr;
        if (tv) { h.tv_sec = tv->tv_sec; h.tv_usec = tv->tv_usec; hp = &h; }
        return select(n, r, w, e, hp);
    });
    reg("inet_addr", +[](const char* s) -> u32 {
        unsigned a, b, cc, d;
        if (sscanf(s, "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return 0xFFFFFFFF;
        return a | (b << 8) | (cc << 16) | (d << 24);
    });
    reg("htonl", +[](u32 v) -> u32 { return __builtin_bswap32(v); });
    reg("ntohl", +[](u32 v) -> u32 { return __builtin_bswap32(v); });
    reg("htons", +[](u32 v) -> u32 { return __builtin_bswap16((u16)v); });
    reg("ntohs", +[](u32 v) -> u32 { return __builtin_bswap16((u16)v); });

    // ------------------------------------------------------------- signals (not delivered to guest code)
    reg("signal", +[](int, u32) -> u32 { return 0; });
    reg("bsd_signal", +[](int, u32) -> u32 { return 0; });
    reg("sigaction", +[](int, void*, void* old) -> int { if (old) memset(old, 0, 16); return 0; });
    reg("sigprocmask", +[](int, void*, void*) -> int { return 0; });
    reg("pthread_sigmask", +[](int, void*, void*) -> int { return 0; });
    reg("sigemptyset", +[](u32* s) -> int { *s = 0; return 0; });
    reg("sigfillset", +[](u32* s) -> int { *s = 0xFFFFFFFF; return 0; });
    reg("sigaddset", +[](u32* s, int n) -> int { *s |= 1u << ((n - 1) & 31); return 0; });
    reg("sigdelset", +[](u32* s, int n) -> int { *s &= ~(1u << ((n - 1) & 31)); return 0; });
    reg("sigismember", +[](const u32* s, int n) -> int { return (*s >> ((n - 1) & 31)) & 1; });
    reg("sigaltstack", +[](void*, void*) -> int { return 0; });
    reg("raise", +[](int sig) -> int { fatal("guest raised signal %d", sig); });
    reg("kill", +[](int pid, int sig) -> int {
        if (pid == getpid() && sig != 0) fatal("guest sent signal %d to itself", sig);
        return 0;
    });
    reg("alarm", +[](u32) -> u32 { return 0; });

    // ------------------------------------------------------------- system properties
    reg("__system_property_get", +[](const char* name, char* value) -> int {
#ifdef __ANDROID__
        return __system_property_get(name, value);
#else
        (void)name;
        value[0] = 0;
        return 0;
#endif
    });
    reg("__system_property_find", +[](const char*) -> u32 { return 0; });
}

}  // namespace a64b
