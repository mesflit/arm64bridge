// POSIX / bionic functions whose structures differ between the 32-bit guest and
// the 64-bit host (epoll, msghdr, iovec, statfs, ifreq, passwd, div_t...) and
// wide-character helpers.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <vector>

#include "guest_types.h"
#include "thunk.h"

namespace a64b {

FILE* guest_file_get(u32 g);

namespace {

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()

// ---- epoll: ARM32 struct epoll_event is { u32 events; u32 pad; u64 data } ----
struct GEpollEvent {
    u32 events;
    u32 pad;
    u64 data;
};
static_assert(sizeof(GEpollEvent) == 16, "guest epoll_event");

// ---- iovec / msghdr ----
struct GIovec { u32 base, len; };
struct GMsghdr { u32 name, namelen, iov, iovlen, control, controllen; i32 flags; };

std::vector<iovec> iov_from_guest(u32 giov, u32 n) {
    std::vector<iovec> v(n);
    const GIovec* g = g2h<GIovec>(giov);
    for (u32 i = 0; i < n; i++) {
        v[i].iov_base = g2h(g[i].base);
        v[i].iov_len = g[i].len;
    }
    return v;
}

// ---- statfs (bionic LP32 layout) ----
struct GStatfs {
    u32 f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    u32 f_namelen, f_frsize, f_flags;
    u32 f_spare[4];
};
static_assert(sizeof(GStatfs) == 88, "guest statfs");

void statfs_to_guest(GStatfs* g, const struct statfs& s) {
    memset(g, 0, sizeof(*g));
    g->f_type = (u32)s.f_type;
    g->f_bsize = (u32)s.f_bsize;
    g->f_blocks = s.f_blocks;
    g->f_bfree = s.f_bfree;
    g->f_bavail = s.f_bavail;
    g->f_files = s.f_files;
    g->f_ffree = s.f_ffree;
    memcpy(g->f_fsid, &s.f_fsid, sizeof(g->f_fsid));
    g->f_namelen = (u32)s.f_namelen;
    g->f_frsize = (u32)s.f_frsize;
    g->f_flags = (u32)s.f_flags;
}

// ---- ifreq: 32 bytes in the guest, 40 on the host (its union holds pointers) ----
constexpr u32 kGuestIfreq = 32;

int socket_ioctl(int fd, u32 req, u32 arg) {
    if (req == SIOCGIFCONF) {
        // struct ifconf { int len; char* buf; }
        u32* gc = g2h<u32>(arg);
        u32 glen = gc[0], gbuf = gc[1];
        u32 n = glen / kGuestIfreq;
        std::vector<ifreq> tmp(n ? n : 1);
        ifconf hc;
        hc.ifc_len = (int)(n * sizeof(ifreq));
        hc.ifc_buf = gbuf ? (char*)tmp.data() : nullptr;
        int r = ioctl(fd, SIOCGIFCONF, &hc);
        if (r < 0) return r;
        u32 got = (u32)hc.ifc_len / sizeof(ifreq);
        if (gbuf)
            for (u32 i = 0; i < got && i < n; i++) memcpy(g2h<u8>(gbuf) + i * kGuestIfreq, &tmp[i], kGuestIfreq);
        gc[0] = got * kGuestIfreq;
        return r;
    }
    if ((req & 0xFF00) == 0x8900 && arg) {
        // Other interface requests take a single struct ifreq (name + sockaddr/int/short).
        ifreq hr;
        memset(&hr, 0, sizeof(hr));
        memcpy(&hr, g2h(arg), kGuestIfreq);
        int r = ioctl(fd, req, &hr);
        memcpy(g2h(arg), &hr, kGuestIfreq);
        return r;
    }
    return ioctl(fd, req, (unsigned long)arg);
}

// ---- wide characters ----
std::string narrow(const u32* w) {
    std::string s;
    for (; w && *w; w++) s.push_back(*w < 128 ? (char)*w : '?');
    return s;
}

int widen_into(u32* out, u32 cap, const std::string& s) {
    if (!cap) return -1;
    // Decode UTF-8 produced by the formatter into code points.
    u32 n = 0;
    for (size_t i = 0; i < s.size() && n + 1 < cap;) {
        unsigned char ch = (unsigned char)s[i];
        u32 cp = ch;
        int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
        if (extra) cp = ch & (0x3F >> extra);
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        out[n++] = cp;
    }
    out[n] = 0;
    return n;
}

const char* const kWctypeNames[] = {nullptr, "alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                    "lower", "print", "punct", "space", "upper", "xdigit"};

int wctype_test(u32 wc, u32 t) {
    switch (t) {
        case 1: return iswalnum(wc);
        case 2: return iswalpha(wc);
        case 3: return iswblank(wc);
        case 4: return iswcntrl(wc);
        case 5: return iswdigit(wc);
        case 6: return iswgraph(wc);
        case 7: return iswlower(wc);
        case 8: return iswprint(wc);
        case 9: return iswpunct(wc);
        case 10: return iswspace(wc);
        case 11: return iswupper(wc);
        case 12: return iswxdigit(wc);
        default: return 0;
    }
}

u32 g_inet_buf = 0, g_tmpnam_buf = 0, g_passwd = 0;
int g_tmpnam_counter = 0;

}  // namespace

void register_posix_extra_thunks() {
    // ---------------------------------------------------------------- epoll / eventfd
    reg("epoll_create", +[](int n) -> int { return epoll_create(n > 0 ? n : 1); });
    reg("epoll_create1", +[](int flags) -> int { return epoll_create1(flags); });
    reg("epoll_ctl", +[](int epfd, int op, int fd, GEpollEvent* ge) -> int {
        epoll_event he;
        memset(&he, 0, sizeof(he));
        if (ge) {
            he.events = ge->events;
            he.data.u64 = ge->data;
        }
        return epoll_ctl(epfd, op, fd, ge ? &he : nullptr);
    });
    auto ewait = +[](int epfd, GEpollEvent* out, int max, int timeout) -> int {
        if (max <= 0) {
            errno = EINVAL;
            return -1;
        }
        std::vector<epoll_event> he((size_t)max);
        int n = epoll_wait(epfd, he.data(), max, timeout);
        for (int i = 0; i < n; i++) {
            out[i].events = he[i].events;
            out[i].pad = 0;
            out[i].data = he[i].data.u64;
        }
        return n;
    };
    static auto s_ewait = ewait;
    reg("epoll_wait", ewait);
    reg("epoll_pwait", +[](int epfd, GEpollEvent* out, int max, int timeout, const void*) -> int { return s_ewait(epfd, out, max, timeout); });
    reg("eventfd", +[](u32 init, int flags) -> int { return eventfd(init, flags); });
    reg("eventfd_read", +[](int fd, u64* v) -> int { return eventfd_read(fd, (eventfd_t*)v); });
    reg("eventfd_write", +[](int fd, unsigned long long v) -> int { return eventfd_write(fd, v); });

    // ---------------------------------------------------------------- scatter/gather I/O
    reg("readv", +[](int fd, u32 giov, int n) -> ssize_t { auto v = iov_from_guest(giov, n > 0 ? n : 0); return readv(fd, v.data(), n); });
    reg("writev", +[](int fd, u32 giov, int n) -> ssize_t { auto v = iov_from_guest(giov, n > 0 ? n : 0); return writev(fd, v.data(), n); });
    auto msg_call = +[](int fd, GMsghdr* g, int flags, int send) -> ssize_t {
        auto v = iov_from_guest(g->iov, g->iovlen);
        msghdr h;
        memset(&h, 0, sizeof(h));
        h.msg_name = g2h(g->name);
        h.msg_namelen = g->namelen;
        h.msg_iov = v.data();
        h.msg_iovlen = v.size();
        if (g->controllen) {
            static bool warned = false;
            if (!warned) LOGW("sendmsg/recvmsg: ancillary data is not bridged and was dropped");
            warned = true;
        }
        ssize_t r = send ? sendmsg(fd, &h, flags) : recvmsg(fd, &h, flags);
        if (!send) {
            g->namelen = h.msg_namelen;
            g->controllen = 0;
            g->flags = h.msg_flags;
        }
        return r;
    };
    static auto s_msg = msg_call;
    reg("sendmsg", +[](int fd, GMsghdr* g, int flags) -> ssize_t { return s_msg(fd, g, flags, 1); });
    reg("recvmsg", +[](int fd, GMsghdr* g, int flags) -> ssize_t { return s_msg(fd, g, flags, 0); });

    RAW(ioctl) {
        ArgReader a(c);
        int fd = A32();
        u32 req = A32(), arg = A32();
        c.set_ret32((u32)socket_ioctl(fd, req, arg));
    });

    // ---------------------------------------------------------------- file systems
    auto do_statfs = +[](const char* p, GStatfs* g) -> int {
        struct statfs s;
        int r = statfs(p, &s);
        if (r == 0) statfs_to_guest(g, s);
        return r;
    };
    auto do_fstatfs = +[](int fd, GStatfs* g) -> int {
        struct statfs s;
        int r = fstatfs(fd, &s);
        if (r == 0) statfs_to_guest(g, s);
        return r;
    };
    reg("statfs", do_statfs);
    reg("statfs64", do_statfs);
    reg("fstatfs", do_fstatfs);
    reg("fstatfs64", do_fstatfs);
    // statvfs has the same field order in bionic LP32 (f_bsize, f_frsize, then 64-bit counters).
    reg("statvfs", +[](const char* p, u32* g) -> int {
        struct statfs s;
        int r = statfs(p, &s);
        if (r != 0) return r;
        memset(g, 0, 96);
        g[0] = (u32)s.f_bsize;
        g[1] = (u32)(s.f_frsize ? s.f_frsize : s.f_bsize);
        u64* q = reinterpret_cast<u64*>(g + 2);
        q[0] = s.f_blocks; q[1] = s.f_bfree; q[2] = s.f_bavail; q[3] = s.f_files; q[4] = s.f_ffree; q[5] = s.f_ffree;
        g[14] = 0;                    // f_fsid
        g[15] = (u32)s.f_flags;       // f_flag
        g[16] = (u32)s.f_namelen;     // f_namemax
        return 0;
    });
    reg("tmpnam", +[](char* out) -> u32 {
        if (!g_tmpnam_buf) g_tmpnam_buf = g_calloc(1, 256);
        char* d = out ? out : g2h<char>(g_tmpnam_buf);
        const char* base = getenv("TMPDIR");
        snprintf(d, 256, "%s/a64b-%d-%d", base && *base ? base : "/data/local/tmp", (int)getpid(), g_tmpnam_counter++);
        return h2g(d);
    });

    // ---------------------------------------------------------------- network names
    reg("gethostname", +[](char* buf, size_t n) -> int { return gethostname(buf, n); });
    reg("inet_ntoa", +[](u32 addr) -> u32 {
        if (!g_inet_buf) g_inet_buf = g_calloc(1, 32);
        in_addr a;
        a.s_addr = addr;
        snprintf(g2h<char>(g_inet_buf), 32, "%s", inet_ntoa(a));
        return g_inet_buf;
    });
    reg("inet_aton", +[](const char* s, u32* out) -> int {
        in_addr a;
        int r = inet_aton(s, &a);
        if (r && out) *out = a.s_addr;
        return r;
    });
    reg("inet_ntop", +[](int af, const void* src, char* dst, u32 n) -> char* { return (char*)inet_ntop(af, src, dst, n); });
    reg("inet_pton", +[](int af, const char* src, void* dst) -> int { return inet_pton(af, src, dst); });
    reg("gethostbyaddr", +[](const void*, u32, int) -> u32 { return 0; });
    // struct addrinfo (LP32): flags, family, socktype, protocol, addrlen, canonname, addr, next
    reg("getaddrinfo", +[](const char* node, const char* service, const i32* ghints, u32* out) -> int {
        addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        if (ghints) {
            hints.ai_flags = ghints[0];
            hints.ai_family = ghints[1];
            hints.ai_socktype = ghints[2];
            hints.ai_protocol = ghints[3];
        }
        int r = getaddrinfo(node, service, ghints ? &hints : nullptr, &res);
        LOGI("getaddrinfo(%s, %s) = %d", node ? node : "", service ? service : "", r);
        if (r != 0) return r;
        u32 head = 0, *link = &head;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            u32 g = g_calloc(1, 32 + (u32)ai->ai_addrlen);
            u32* f = g2h<u32>(g);
            f[0] = (u32)ai->ai_flags;
            f[1] = (u32)ai->ai_family;
            f[2] = (u32)ai->ai_socktype;
            f[3] = (u32)ai->ai_protocol;
            f[4] = (u32)ai->ai_addrlen;
            f[5] = ai->ai_canonname ? g_strdup(ai->ai_canonname) : 0;
            f[6] = g + 32;
            memcpy(g2h(g + 32), ai->ai_addr, ai->ai_addrlen);
            f[7] = 0;
            *link = g;
            link = &f[7];
        }
        freeaddrinfo(res);
        *out = head;
        return 0;
    });
    reg("freeaddrinfo", +[](u32 g) {
        while (g) {
            u32* f = g2h<u32>(g);
            u32 next = f[7];
            if (f[5]) g_free(f[5]);
            g_free(g);
            g = next;
        }
    });
    reg("gai_strerror", +[](int e) -> u32 { return guest_intern_string(gai_strerror(e)); });
    // struct hostent (LP32): name, aliases, addrtype, length, addr_list. One buffer per thread.
    reg("gethostbyname", +[](const char* name) -> u32 {
        addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        int r = getaddrinfo(name, nullptr, &hints, &res);
        LOGI("gethostbyname(%s) = %d", name ? name : "", r);
        if (r != 0 || !res) return 0;
        static thread_local u32 buf = 0;
        if (!buf) buf = g_calloc(1, 64 + 16 * 4 + 16 * 4 + 256);
        u32* he = g2h<u32>(buf);
        u32 addrs = buf + 64, list = buf + 64 + 64, name_g = buf + 64 + 128;
        snprintf(g2h<char>(name_g), 256, "%s", name);
        int n = 0;
        for (addrinfo* ai = res; ai && n < 15; ai = ai->ai_next) {
            if (ai->ai_family != AF_INET) continue;
            memcpy(g2h(addrs + n * 4), &reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr, 4);
            g2h<u32>(list)[n] = addrs + n * 4;
            n++;
        }
        g2h<u32>(list)[n] = 0;
        freeaddrinfo(res);
        he[0] = name_g;
        he[1] = list + n * 4;  // empty alias list (points at the terminating NULL)
        he[2] = AF_INET;
        he[3] = 4;
        he[4] = list;
        return n ? buf : 0;
    });

    // ---------------------------------------------------------------- users
    auto pw = +[]() -> u32 {
        if (!g_passwd) {
            g_passwd = g_calloc(6, 4);
            u32* p = g2h<u32>(g_passwd);
            p[0] = g_strdup("app");
            p[1] = 0;
            p[2] = getuid();
            p[3] = getgid();
            p[4] = g_strdup("/");
            p[5] = g_strdup("/system/bin/sh");
        }
        return g_passwd;
    };
    static auto s_pw = pw;
    reg("getpwuid", +[](u32) -> u32 { return s_pw(); });
    reg("getpwnam", +[](const char*) -> u32 { return s_pw(); });
    reg("getlogin", +[]() -> u32 { return g2h<u32>(s_pw())[0]; });

    // ---------------------------------------------------------------- div_t / ldiv_t: returned in memory (r0 = result)
    RAW(div) {
        i32 n = (i32)c.r[1], d = (i32)c.r[2];
        u32* out = g2h<u32>(c.r[0]);
        out[0] = d ? (u32)(n / d) : 0;
        out[1] = d ? (u32)(n % d) : 0;
    });
    RAW(ldiv) {
        i32 n = (i32)c.r[1], d = (i32)c.r[2];
        u32* out = g2h<u32>(c.r[0]);
        out[0] = d ? (u32)(n / d) : 0;
        out[1] = d ? (u32)(n % d) : 0;
    });
    RAW(lldiv) {
        // r0 = result, (r2,r3) = numerator, stack = denominator
        ArgReader a(c);
        u32 out = A32();
        i64 n = (i64)a.next64(), d = (i64)a.next64();
        i64 q = d ? n / d : 0, r = d ? n % d : 0;
        memcpy(g2h(out), &q, 8);
        memcpy(g2h(out + 8), &r, 8);
    });

    // ---------------------------------------------------------------- time parsing
    reg("strptime", +[](const char* s, const char* fmt, GTm* g) -> u32 {
        struct tm t = from_guest(g);
        char* end = strptime(s, fmt, &t);
        to_guest(g, t, g->tm_zone);
        return h2g(end);
    });

    // ---------------------------------------------------------------- wide characters
    reg("wctype", +[](const char* name) -> u32 {
        for (u32 i = 1; i < sizeof(kWctypeNames) / sizeof(kWctypeNames[0]); i++)
            if (strcmp(name, kWctypeNames[i]) == 0) return i;
        return 0;
    });
    reg("iswctype", +[](u32 wc, u32 t) -> int { return wctype_test(wc, t); });
    reg("wcsxfrm", +[](u32* d, const u32* s, u32 n) -> u32 {
        u32 len = 0;
        while (s[len]) len++;
        if (n > len) memcpy(d, s, (len + 1) * 4);
        return len;
    });
    RAW(swprintf) {
        ArgReader a(c);
        u32* out = g2h<u32>(A32());
        u32 cap = A32();
        std::string f = narrow(g2h<u32>(A32()));
        VaSource va(a);
        std::string s = guest_format(f.c_str(), va);
        int n = widen_into(out, cap, s);
        c.set_ret32((u32)((n >= 0 && (u32)n < s.size() && (u32)n + 1 >= cap) ? -1 : n));
    });
    RAW(vswprintf) {
        ArgReader a(c);
        u32* out = g2h<u32>(A32());
        u32 cap = A32();
        std::string f = narrow(g2h<u32>(A32()));
        VaSource va(A32());
        std::string s = guest_format(f.c_str(), va);
        int n = widen_into(out, cap, s);
        c.set_ret32((u32)((n >= 0 && (u32)n < s.size() && (u32)n + 1 >= cap) ? -1 : n));
    });
    reg("wcsftime", +[](u32* out, u32 cap, const u32* fmt, const GTm* g) -> u32 {
        struct tm t = from_guest(g);
        char buf[512];
        size_t n = strftime(buf, sizeof(buf), narrow(fmt).c_str(), &t);
        if (n == 0 || n >= cap) return 0;
        return (u32)widen_into(out, cap, std::string(buf, n));
    });
    auto getwc_fn = +[](u32 g) -> u32 { FILE* f = guest_file_get(g); return f ? (u32)fgetwc(f) : (u32)WEOF; };
    auto putwc_fn = +[](u32 wc, u32 g) -> u32 { FILE* f = guest_file_get(g); return f ? (u32)fputwc((wchar_t)wc, f) : (u32)WEOF; };
    reg("getwc", getwc_fn);
    reg("fgetwc", getwc_fn);
    reg("putwc", putwc_fn);
    reg("fputwc", putwc_fn);
    reg("ungetwc", +[](u32 wc, u32 g) -> u32 { FILE* f = guest_file_get(g); return f ? (u32)ungetwc(wc, f) : (u32)WEOF; });
    reg("wcstol", +[](const u32* s, u32* end, int base) -> i32 {
        std::string n = narrow(s);
        char* e = nullptr;
        long v = strtol(n.c_str(), &e, base);
        if (end) *end = h2g(s) + (u32)(e - n.c_str()) * 4;
        return (i32)v;
    });
    reg("wcstod", +[](const u32* s, u32* end) -> double {
        std::string n = narrow(s);
        char* e = nullptr;
        double v = strtod(n.c_str(), &e);
        if (end) *end = h2g(s) + (u32)(e - n.c_str()) * 4;
        return v;
    });
}

}  // namespace a64b
