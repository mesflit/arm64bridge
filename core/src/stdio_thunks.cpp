// stdio: FILE handles, printf/scanf families with guest varargs.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <unistd.h>

#include <mutex>
#include <unordered_map>

#include "thunk.h"

namespace a64b {

// ---------------------------------------------------------------------------
// printf-style formatting. Guest sizes: int/long/size_t/pointer = 32 bit,
// long long = 64 bit, double = 64 bit (8-byte aligned).
// ---------------------------------------------------------------------------
std::string guest_format(const char* fmt, VaSource& va) {
    std::string out;
    if (!fmt) return out;
    char buf[512];
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            const char* q = strchr(p, '%');
            if (!q) q = p + strlen(p);
            out.append(p, q - p);
            p = q;
            continue;
        }
        p++;
        if (*p == '%') {
            out.push_back('%');
            p++;
            continue;
        }
        std::string spec = "%";
        while (*p && strchr("-+ #0'", *p)) spec.push_back(*p++);
        if (*p == '*') {
            spec += std::to_string((i32)va.next32());
            p++;
        } else {
            while (*p >= '0' && *p <= '9') spec.push_back(*p++);
        }
        if (*p == '.') {
            spec.push_back(*p++);
            if (*p == '*') {
                i32 prec = (i32)va.next32();
                if (prec >= 0) spec += std::to_string(prec); else spec.pop_back();
                p++;
            } else {
                while (*p >= '0' && *p <= '9') spec.push_back(*p++);
            }
        }
        bool is64 = false, is_wide = false;
        for (;; p++) {
            if (*p == 'l') {
                if (p[1] == 'l') { is64 = true; p++; } else is_wide = true;
            } else if (*p == 'q' || *p == 'j') {
                is64 = true;
            } else if (*p == 'h' || *p == 'z' || *p == 't' || *p == 'L') {
            } else {
                break;
            }
        }
        char conv = *p;
        if (!conv) break;
        p++;
        switch (conv) {
            case 'd': case 'i': {
                long long v = is64 ? (long long)va.next64() : (long long)(i32)va.next32();
                spec += "lld";
                snprintf(buf, sizeof(buf), spec.c_str(), v);
                out += buf;
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                unsigned long long v = is64 ? va.next64() : va.next32();
                spec += "ll";
                spec.push_back(conv);
                snprintf(buf, sizeof(buf), spec.c_str(), v);
                out += buf;
                break;
            }
            case 'c': {
                spec.push_back('c');
                snprintf(buf, sizeof(buf), spec.c_str(), (int)va.next32());
                out += buf;
                break;
            }
            case 'p': {
                snprintf(buf, sizeof(buf), "0x%x", va.next32());
                out += buf;
                break;
            }
            case 's': {
                u32 gp = va.next32();
                std::string tmp;
                const char* s = "(null)";
                if (gp && is_wide) {
                    for (const u32* w = g2h<u32>(gp); *w; w++) tmp.push_back(*w < 128 ? (char)*w : '?');
                    s = tmp.c_str();
                } else if (gp) {
                    s = g2h<char>(gp);
                }
                spec.push_back('s');
                int n = snprintf(nullptr, 0, spec.c_str(), s);
                if (n > 0) {
                    size_t old = out.size();
                    out.resize(old + n + 1);
                    snprintf(&out[old], n + 1, spec.c_str(), s);
                    out.resize(old + n);
                }
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                u64 bits = va.next64();
                double d;
                memcpy(&d, &bits, 8);
                spec.push_back(conv);
                int n = snprintf(buf, sizeof(buf), spec.c_str(), d);
                if (n >= (int)sizeof(buf)) {
                    std::string big(n + 1, 0);
                    snprintf(big.data(), n + 1, spec.c_str(), d);
                    out.append(big.c_str());
                } else {
                    out += buf;
                }
                break;
            }
            case 'n': {
                u32 gp = va.next32();
                if (gp) *g2h<i32>(gp) = (i32)out.size();
                break;
            }
            default:
                out += spec;
                out.push_back(conv);
                break;
        }
    }
    return out;
}

namespace {

// ---- FILE* mapping: the guest sees the address of a dummy block ----
constexpr u32 kGuestFileSize = 84;  // sizeof(FILE) in 32-bit bionic
std::mutex g_file_mu;
std::unordered_map<u32, FILE*>& files() {
    static auto* m = new std::unordered_map<u32, FILE*>();
    return *m;
}
u32 g_sF = 0;

FILE* hf(u32 g) {
    if (!g) return nullptr;
    if (g_sF && g >= g_sF && g < g_sF + 3 * kGuestFileSize) {
        switch ((g - g_sF) / kGuestFileSize) {
            case 0: return stdin;
            case 1: return stdout;
            default: return stderr;
        }
    }
    std::lock_guard<std::mutex> lk(g_file_mu);
    auto it = files().find(g);
    if (it == files().end()) {
        LOGW("guest used an unknown FILE* 0x%08x", g);
        return nullptr;
    }
    return it->second;
}

u32 new_file(FILE* f) {
    if (!f) return 0;
    u32 g = g_calloc(1, kGuestFileSize);
    // Old bionic headers read the descriptor straight from the structure (fileno() as a macro):
    // struct __sFILE { unsigned char* _p; int _r; int _w; short _flags; short _file; ... }.
    *g2h<i16>(g + 12) = 0x10;  // __SRW
    *g2h<i16>(g + 14) = (i16)fileno(f);
    std::lock_guard<std::mutex> lk(g_file_mu);
    files()[g] = f;
    return g;
}

FILE* drop_file(u32 g) {
    std::lock_guard<std::mutex> lk(g_file_mu);
    auto it = files().find(g);
    if (it == files().end()) return nullptr;
    FILE* f = it->second;
    files().erase(it);
    return f;
}

void emit(FILE* f, const std::string& s) {
#ifdef __ANDROID__
    if (f == stdout || f == stderr) {
        log_tagged(LOG_INFO, f == stdout ? "guest-stdout" : "guest-stderr", s.c_str());
        return;
    }
#endif
    if (f) fwrite(s.data(), 1, s.size(), f);
}

u32 store(char* dst, size_t cap, bool bounded, const std::string& s) {
    if (!bounded) {
        memcpy(dst, s.c_str(), s.size() + 1);
    } else if (cap) {
        size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        memcpy(dst, s.data(), n);
        dst[n] = 0;
    }
    return (u32)s.size();
}

// Rewrites a scanf format so that every conversion stores a value of the size
// the guest expects, then lets the host scanf write straight into guest memory.
std::string scanf_format(const char* fmt, int* nargs) {
    std::string out;
    *nargs = 0;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out.push_back(*p++);
            continue;
        }
        out.push_back(*p++);
        if (*p == '%') {
            out.push_back(*p++);
            continue;
        }
        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            out.push_back(*p++);
        }
        while (*p >= '0' && *p <= '9') out.push_back(*p++);
        bool is64 = false, is_short = false, is_char = false, is_l = false;
        for (;; p++) {
            if (*p == 'l') {
                if (p[1] == 'l') { is64 = true; p++; } else is_l = true;
            } else if (*p == 'q' || *p == 'j') {
                is64 = true;
            } else if (*p == 'h') {
                if (p[1] == 'h') { is_char = true; p++; } else is_short = true;
            } else if (*p == 'z' || *p == 't') {
            } else if (*p == 'L') {
                is_l = true;
            } else {
                break;
            }
        }
        char conv = *p;
        if (!conv) break;
        p++;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'n':
                if (is64) out += "ll"; else if (is_char) out += "hh"; else if (is_short) out += "h";
                out.push_back(conv);
                break;
            case 'f': case 'e': case 'g': case 'E': case 'G': case 'a':
                if (is_l) out += "l";
                out.push_back(conv);
                break;
            case 'p':
                out.push_back('x');
                break;
            case '[':
                out.push_back('[');
                if (*p == '^') out.push_back(*p++);
                if (*p == ']') out.push_back(*p++);
                while (*p && *p != ']') out.push_back(*p++);
                if (*p == ']') out.push_back(*p++);
                break;
            default:  // s, c
                out.push_back(conv);
                break;
        }
        if (!suppress) (*nargs)++;
    }
    return out;
}

enum { kMaxScan = 20 };

template <class Call>
int do_scan(const char* fmt, VaSource& va, Call call) {
    int nargs = 0;
    std::string hfmt = scanf_format(fmt, &nargs);
    if (nargs > kMaxScan) {
        LOGW("scanf with %d conversions is not supported", nargs);
        return -1;
    }
    void* p[kMaxScan] = {};
    for (int i = 0; i < nargs; i++) p[i] = g2h(va.next32());
    return call(hfmt.c_str(), p);
}

#define SCAN_ARGS p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19]

}  // namespace

u32 guest_file_new(FILE* f) { return new_file(f); }
FILE* guest_file_get(u32 g) { return hf(g); }

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()

void register_stdio_thunks() {
    // Standard streams: old NDKs reference the __sF array, newer ones the stdin/stdout/stderr pointers.
    g_sF = g_calloc(3, kGuestFileSize);
    for (int i = 0; i < 3; i++) {  // descriptor field of stdin, stdout and stderr (see new_file)
        *g2h<i16>(g_sF + i * kGuestFileSize + 12) = i == 0 ? 0x4 : 0x8;  // __SRD / __SWR
        *g2h<i16>(g_sF + i * kGuestFileSize + 14) = (i16)i;
    }
    thunk_register_data("__sF", g_sF);
    const char* names[] = {"stdin", "stdout", "stderr"};
    for (int i = 0; i < 3; i++) {
        u32 var = g_calloc(1, 4);
        *g2h<u32>(var) = g_sF + i * kGuestFileSize;
        thunk_register_data(names[i], var);
    }

    auto open_file = +[](const char* p, const char* m) -> u32 {
        int v = guest_proc_open(p);
        if (v == -2) return new_file(fopen(p, m));
        if (v < 0) return 0;
        FILE* f = fdopen(v, "r");
        if (!f) close(v);
        return new_file(f);
    };
    reg("fopen", open_file);
    reg("fopen64", open_file);
    reg("fdopen", +[](int fd, const char* m) -> u32 { return new_file(fdopen(fd, m)); });
    reg("tmpfile", +[]() -> u32 { return new_file(tmpfile()); });
    reg("freopen", +[](const char* p, const char* m, u32 g) -> u32 {
        FILE* f = hf(g);
        if (!f) return 0;
        FILE* n = freopen(p, m, f);
        if (!n) {
            drop_file(g);
            return 0;
        }
        return g;
    });
    reg("fclose", +[](u32 g) -> int {
        FILE* f = drop_file(g);
        if (!f) return hf(g) ? 0 : -1;
        g_free(g);
        return fclose(f);
    });
    reg("fread", +[](void* b, size_t s, size_t n, u32 g) -> size_t { FILE* f = hf(g); return f ? fread(b, s, n, f) : 0; });
    reg("fwrite", +[](const void* b, size_t s, size_t n, u32 g) -> size_t {
        FILE* f = hf(g);
        if (!f) return 0;
#ifdef __ANDROID__
        if (f == stdout || f == stderr) {
            emit(f, std::string((const char*)b, s * n));
            return n;
        }
#endif
        return fwrite(b, s, n, f);
    });
    reg("fseek", +[](u32 g, i32 off, int wh) -> int { FILE* f = hf(g); return f ? fseek(f, off, wh) : -1; });
    reg("fseeko", +[](u32 g, i32 off, int wh) -> int { FILE* f = hf(g); return f ? fseeko(f, off, wh) : -1; });
    reg("fseeko64", +[](u32 g, long long off, int wh) -> int { FILE* f = hf(g); return f ? fseeko(f, off, wh) : -1; });
    reg("ftell", +[](u32 g) -> i32 { FILE* f = hf(g); return f ? (i32)ftell(f) : -1; });
    reg("ftello", +[](u32 g) -> i32 { FILE* f = hf(g); return f ? (i32)ftello(f) : -1; });
    reg("ftello64", +[](u32 g) -> long long { FILE* f = hf(g); return f ? (long long)ftello(f) : -1; });
    reg("rewind", +[](u32 g) { if (FILE* f = hf(g)) rewind(f); });
    reg("fgetpos", +[](u32 g, i32* pos) -> int { FILE* f = hf(g); if (!f) return -1; *pos = (i32)ftell(f); return 0; });
    reg("fsetpos", +[](u32 g, const i32* pos) -> int { FILE* f = hf(g); return f ? fseek(f, *pos, SEEK_SET) : -1; });
    reg("fflush", +[](u32 g) -> int { return g ? (hf(g) ? fflush(hf(g)) : -1) : fflush(nullptr); });
    reg("feof", +[](u32 g) -> int { FILE* f = hf(g); return f ? feof(f) : 1; });
    reg("ferror", +[](u32 g) -> int { FILE* f = hf(g); return f ? ferror(f) : 1; });
    reg("clearerr", +[](u32 g) { if (FILE* f = hf(g)) clearerr(f); });
    reg("fileno", +[](u32 g) -> int { FILE* f = hf(g); return f ? fileno(f) : -1; });
    reg("fgets", +[](char* b, int n, u32 g) -> char* { FILE* f = hf(g); return f ? fgets(b, n, f) : nullptr; });
    reg("fgetc", +[](u32 g) -> int { FILE* f = hf(g); return f ? fgetc(f) : EOF; });
    reg("__srget", +[](u32 g) -> int { FILE* f = hf(g); return f ? fgetc(f) : EOF; });  // getc() macro refill
    reg("__swbuf", +[](int ch, u32 g) -> int { FILE* f = hf(g); return f ? fputc(ch, f) : EOF; });
    reg("getc", +[](u32 g) -> int { FILE* f = hf(g); return f ? fgetc(f) : EOF; });
    reg("ungetc", +[](int ch, u32 g) -> int { FILE* f = hf(g); return f ? ungetc(ch, f) : EOF; });
    reg("fputc", +[](int ch, u32 g) -> int { FILE* f = hf(g); if (!f) return EOF; emit(f, std::string(1, (char)ch)); return ch; });
    reg("putc", +[](int ch, u32 g) -> int { FILE* f = hf(g); if (!f) return EOF; emit(f, std::string(1, (char)ch)); return ch; });
    reg("fputs", +[](const char* s, u32 g) -> int { FILE* f = hf(g); if (!f) return EOF; emit(f, s); return 0; });
    reg("puts", +[](const char* s) -> int { emit(stdout, std::string(s) + "\n"); return 0; });
    reg("putchar", +[](int ch) -> int { emit(stdout, std::string(1, (char)ch)); return ch; });
    reg("getchar", +[]() -> int { return EOF; });
    reg("perror", +[](const char* s) { emit(stderr, std::string(s ? s : "") + ": " + strerror(errno) + "\n"); });
    reg("setvbuf", +[](u32, u32, int, u32) -> int { return 0; });
    reg("setbuf", +[](u32, u32) {});
    reg("setbuffer", +[](u32, u32, u32) {});
    reg("flockfile", +[](u32) {});
    reg("funlockfile", +[](u32) {});

    // ---- printf family ----
    RAW(printf) { ArgReader a(c); const char* f = g2h<char>(A32()); VaSource va(a); std::string s = guest_format(f, va); emit(stdout, s); c.set_ret32((u32)s.size()); });
    RAW(vprintf) { ArgReader a(c); const char* f = g2h<char>(A32()); VaSource va(A32()); std::string s = guest_format(f, va); emit(stdout, s); c.set_ret32((u32)s.size()); });
    RAW(fprintf) { ArgReader a(c); FILE* fp = hf(A32()); const char* f = g2h<char>(A32()); VaSource va(a); std::string s = guest_format(f, va); emit(fp, s); c.set_ret32((u32)s.size()); });
    RAW(vfprintf) { ArgReader a(c); FILE* fp = hf(A32()); const char* f = g2h<char>(A32()); VaSource va(A32()); std::string s = guest_format(f, va); emit(fp, s); c.set_ret32((u32)s.size()); });
    RAW(__fprintf_chk) { ArgReader a(c); FILE* fp = hf(A32()); A32(); const char* f = g2h<char>(A32()); VaSource va(a); std::string s = guest_format(f, va); emit(fp, s); c.set_ret32((u32)s.size()); });
    RAW(sprintf) { ArgReader a(c); char* d = g2h<char>(A32()); const char* f = g2h<char>(A32()); VaSource va(a); c.set_ret32(store(d, 0, false, guest_format(f, va))); });
    RAW(vsprintf) { ArgReader a(c); char* d = g2h<char>(A32()); const char* f = g2h<char>(A32()); VaSource va(A32()); c.set_ret32(store(d, 0, false, guest_format(f, va))); });
    RAW(snprintf) { ArgReader a(c); char* d = g2h<char>(A32()); u32 n = A32(); const char* f = g2h<char>(A32()); VaSource va(a); c.set_ret32(store(d, n, true, guest_format(f, va))); });
    RAW(vsnprintf) { ArgReader a(c); char* d = g2h<char>(A32()); u32 n = A32(); const char* f = g2h<char>(A32()); VaSource va(A32()); c.set_ret32(store(d, n, true, guest_format(f, va))); });
    // fortified variants: (dst, [size,] flags, dst_size, fmt, ...)
    RAW(__sprintf_chk) { ArgReader a(c); char* d = g2h<char>(A32()); A32(); A32(); const char* f = g2h<char>(A32()); VaSource va(a); c.set_ret32(store(d, 0, false, guest_format(f, va))); });
    RAW(__vsprintf_chk) { ArgReader a(c); char* d = g2h<char>(A32()); A32(); A32(); const char* f = g2h<char>(A32()); VaSource va(A32()); c.set_ret32(store(d, 0, false, guest_format(f, va))); });
    RAW(__snprintf_chk) { ArgReader a(c); char* d = g2h<char>(A32()); u32 n = A32(); A32(); A32(); const char* f = g2h<char>(A32()); VaSource va(a); c.set_ret32(store(d, n, true, guest_format(f, va))); });
    RAW(__vsnprintf_chk) { ArgReader a(c); char* d = g2h<char>(A32()); u32 n = A32(); A32(); A32(); const char* f = g2h<char>(A32()); VaSource va(A32()); c.set_ret32(store(d, n, true, guest_format(f, va))); });
    RAW(asprintf) { ArgReader a(c); u32* out = g2h<u32>(A32()); const char* f = g2h<char>(A32()); VaSource va(a); std::string s = guest_format(f, va); *out = g_strdup(s.c_str()); c.set_ret32((u32)s.size()); });
    RAW(vasprintf) { ArgReader a(c); u32* out = g2h<u32>(A32()); const char* f = g2h<char>(A32()); VaSource va(A32()); std::string s = guest_format(f, va); *out = g_strdup(s.c_str()); c.set_ret32((u32)s.size()); });

    // ---- scanf family ----
    RAW(sscanf) { ArgReader a(c); const char* s = g2h<char>(A32()); const char* f = g2h<char>(A32()); VaSource va(a);
        c.set_ret32((u32)do_scan(f, va, [&](const char* hfmt, void** p) { return sscanf(s, hfmt, SCAN_ARGS); })); });
    RAW(vsscanf) { ArgReader a(c); const char* s = g2h<char>(A32()); const char* f = g2h<char>(A32()); VaSource va(A32());
        c.set_ret32((u32)do_scan(f, va, [&](const char* hfmt, void** p) { return sscanf(s, hfmt, SCAN_ARGS); })); });
    RAW(fscanf) { ArgReader a(c); FILE* fp = hf(A32()); const char* f = g2h<char>(A32()); VaSource va(a);
        c.set_ret32(fp ? (u32)do_scan(f, va, [&](const char* hfmt, void** p) { return fscanf(fp, hfmt, SCAN_ARGS); }) : (u32)-1); });
    RAW(vfscanf) { ArgReader a(c); FILE* fp = hf(A32()); const char* f = g2h<char>(A32()); VaSource va(A32());
        c.set_ret32(fp ? (u32)do_scan(f, va, [&](const char* hfmt, void** p) { return fscanf(fp, hfmt, SCAN_ARGS); }) : (u32)-1); });
}

}  // namespace a64b
