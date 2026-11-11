// Dynamic linker API, ARM EABI helpers, setjmp/longjmp, raw system calls and
// data symbols exported by bionic.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <ctype.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <unicorn/unicorn.h>

#include <cmath>
#include <mutex>

#include "elf_loader.h"
#include "guest_types.h"
#include "syscalls.h"
#include "thunk.h"

namespace a64b {

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()

namespace {

constexpr u32 kHandleBridge = 0x7FFF0001;  // dlopen() of a library implemented by the bridge
thread_local std::string t_dlerror;
thread_local bool t_dlerror_set = false;
u32 g_dlerror_buf = 0;

void set_dlerror(const std::string& e) {
    t_dlerror = e;
    t_dlerror_set = true;
}

Module* module_from_handle(u32 h) {
    for (Module* m : loader_modules())
        if (m->base == h) return m;
    return nullptr;
}

const int kDRegs[8] = {UC_ARM_REG_D8, UC_ARM_REG_D9, UC_ARM_REG_D10, UC_ARM_REG_D11,
                       UC_ARM_REG_D12, UC_ARM_REG_D13, UC_ARM_REG_D14, UC_ARM_REG_D15};

// jmp_buf layout used by the bridge: [0..7] r4-r11, [8] sp, [9] lr, [10] magic, [12..27] d8-d15
void do_setjmp(Cpu& c) {
    u32* buf = g2h<u32>(c.r[0]);
    for (int i = 0; i < 8; i++) buf[i] = c.r[4 + i];
    buf[8] = c.r[13];
    buf[9] = c.r[14];
    buf[10] = 0xA64B5E7A;
    for (int i = 0; i < 8; i++) uc_reg_read(c.uc, kDRegs[i], &buf[12 + i * 2]);
    c.set_ret32(0);
}

void do_longjmp(Cpu& c) {
    u32* buf = g2h<u32>(c.r[0]);
    u32 val = c.r[1];
    if (buf[10] != 0xA64B5E7A) fatal("longjmp with a jmp_buf that was not filled by setjmp");
    for (int i = 0; i < 8; i++) c.r[4 + i] = buf[i];
    c.r[13] = buf[8];
    c.r[14] = buf[9];
    c.r[15] = buf[9];
    for (int i = 0; i < 8; i++) uc_reg_write(c.uc, kDRegs[i], &buf[12 + i * 2]);
    c.r[0] = val ? val : 1;
    c.regs_dirty = true;
    c.pc_override = true;
}

double as_d(u64 v) { double d; memcpy(&d, &v, 8); return d; }
float as_f(u32 v) { float f; memcpy(&f, &v, 4); return f; }
u64 bits(double d) { u64 v; memcpy(&v, &d, 8); return v; }
u32 bits(float f) { u32 v; memcpy(&v, &f, 4); return v; }
u64 r01(Cpu& c) { return (u64)c.r[0] | ((u64)c.r[1] << 32); }
u64 r23(Cpu& c) { return (u64)c.r[2] | ((u64)c.r[3] << 32); }

}  // namespace

u32 guest_syscall_args(Cpu& c, u32 nr, const u32 a[6]) {
    auto wrap = [](long r) -> u32 { return r < 0 ? (u32)-errno : (u32)r; };
    switch (nr) {
        case 1:    // exit
        case 248:  // exit_group
            LOGW("guest exit(%d) via syscall", (int)a[0]);
            _exit((int)a[0]);
        case 3: return wrap(read((int)a[0], g2h(a[1]), a[2]));
        case 4: return wrap(write((int)a[0], g2h(a[1]), a[2]));
        case 6: return wrap(close((int)a[0]));
        case 20: return (u32)getpid();
        case 158: sched_yield(); return 0;
        case 224: return (u32)syscall(SYS_gettid);
        case 78: {  // gettimeofday
            timeval tv;
            gettimeofday(&tv, nullptr);
            if (a[0]) { g2h<GTimeval>(a[0])->tv_sec = (i32)tv.tv_sec; g2h<GTimeval>(a[0])->tv_usec = (i32)tv.tv_usec; }
            return 0;
        }
        case 263: {  // clock_gettime
            timespec ts;
            if (clock_gettime((clockid_t)a[0], &ts) != 0) return (u32)-errno;
            if (a[1]) to_guest(g2h<GTimespec>(a[1]), ts);
            return 0;
        }
        case 240: {  // futex(uaddr, op, val, timeout|val2, uaddr2, val3)
            int cmd = a[1] & 0x7f;
            timespec ts, *tsp = nullptr;
            unsigned long a3 = a[3];
            if ((cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) && a[3]) {
                ts = from_guest(g2h<GTimespec>(a[3]));
                tsp = &ts;
                a3 = (unsigned long)tsp;
            }
            long r = syscall(SYS_futex, g2h(a[0]), (int)a[1], (int)a[2], a3, g2h(a[4]), (int)a[5]);
            return wrap(r);
        }
        case 0xf0002:  // ARM cacheflush
            mem_bump_code_epoch();
            return 0;
        case 0xf0005:  // ARM set_tls
            uc_reg_write(c.uc, UC_ARM_REG_C13_C0_3, &a[0]);
            return 0;
        default:
            LOGW("unsupported guest system call %u", nr);
            return (u32)-ENOSYS;
    }
}

u32 guest_syscall(Cpu& c, u32 nr) {
    u32 args[6] = {c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5]};
    return guest_syscall_args(c, nr, args);
}

void register_data_symbols() {
    u32 guard = g_calloc(1, 4);
    *g2h<u32>(guard) = 0x5AFE0000u | ((u32)getpid() << 4) | 0xA;
    thunk_register_data("__stack_chk_guard", guard);

    u32 env = g_calloc(1, 8);  // char** environ -> { NULL }
    u32 envp = g_calloc(1, 4);
    *g2h<u32>(envp) = env;
    thunk_register_data("environ", envp);
    u32 prog = g_calloc(1, 4);
    *g2h<u32>(prog) = g_strdup("app_process");
    thunk_register_data("__progname", prog);

    // bionic's classic ctype tables: _ctype_ points to 1 + 256 flag bytes.
    u32 ct = g_calloc(1, 260);
    u8* t = g2h<u8>(ct);
    for (int ch = 0; ch < 128; ch++) {
        u8 f = 0;
        if (isupper(ch)) f |= 0x01;
        if (islower(ch)) f |= 0x02;
        if (isdigit(ch)) f |= 0x04;
        if (isspace(ch)) f |= 0x08;
        if (ispunct(ch)) f |= 0x10;
        if (iscntrl(ch)) f |= 0x20;
        if (isxdigit(ch)) f |= 0x40;
        if (ch == ' ') f |= 0x80;
        t[ch + 1] = f;
    }
    u32 ctp = g_calloc(1, 4);
    *g2h<u32>(ctp) = ct;
    thunk_register_data("_ctype_", ctp);
    u32 lo = g_calloc(257, 2), up = g_calloc(257, 2);
    for (int ch = 0; ch < 256; ch++) {
        g2h<i16>(lo)[ch + 1] = (i16)(ch < 128 ? tolower(ch) : ch);
        g2h<i16>(up)[ch + 1] = (i16)(ch < 128 ? toupper(ch) : ch);
    }
    g2h<i16>(lo)[0] = g2h<i16>(up)[0] = -1;
    u32 lop = g_calloc(1, 4), upp = g_calloc(1, 4);
    *g2h<u32>(lop) = lo;
    *g2h<u32>(upp) = up;
    thunk_register_data("_tolower_tab_", lop);
    thunk_register_data("_toupper_tab_", upp);
}

void register_misc_thunks() {
    // ---------------------------------------------------------------- libdl
    reg("dlopen", +[](const char* name, int) -> u32 {
        if (!name) return kHandleBridge;
        std::string n = name;
        size_t slash = n.rfind('/');
        if (slash != std::string::npos) n = n.substr(slash + 1);
        if (loader_is_system_lib(n)) return kHandleBridge;
        std::string err;
        Module* m = loader_load(n, &err);
        if (!m) {
            set_dlerror(err);
            return 0;
        }
        return m->base;
    });
    reg("dlclose", +[](u32) -> int { return 0; });
    reg("dlsym", +[](u32 handle, const char* name) -> u32 {
        u32 a = 0;
        if (Module* m = module_from_handle(handle)) a = m->find(name);
        if (!a && (handle == 0 || handle >= 0xFFFFFFFE || handle == kHandleBridge || !module_from_handle(handle))) {
            if (handle != kHandleBridge) a = loader_lookup(name);
            if (!a) a = thunk_resolve(name);
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

    // ---------------------------------------------------------------- setjmp / longjmp
    for (const char* n : {"setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp"}) reg_raw(n, do_setjmp);
    for (const char* n : {"longjmp", "_longjmp", "siglongjmp"}) reg_raw(n, do_longjmp);

    // ---------------------------------------------------------------- ARM EABI run-time helpers
    for (const char* n : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8"})
        reg(n, +[](void* d, const void* s, size_t len) { memcpy(d, s, len); });
    for (const char* n : {"__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"})
        reg(n, +[](void* d, const void* s, size_t len) { memmove(d, s, len); });
    for (const char* n : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"})
        reg(n, +[](void* d, size_t len, int v) { memset(d, v, len); });
    for (const char* n : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"})
        reg(n, +[](void* d, size_t len) { memset(d, 0, len); });

    for (const char* n : {"__aeabi_idiv", "__divsi3"})
        reg(n, +[](i32 x, i32 y) -> i32 { return y == 0 ? 0 : (y == -1 ? (i32)(0u - (u32)x) : x / y); });
    for (const char* n : {"__aeabi_uidiv", "__udivsi3"}) reg(n, +[](u32 x, u32 y) -> u32 { return y ? x / y : 0; });
    reg("__modsi3", +[](i32 x, i32 y) -> i32 { return (y == 0 || y == -1) ? 0 : x % y; });
    reg("__umodsi3", +[](u32 x, u32 y) -> u32 { return y ? x % y : 0; });
    RAW(__aeabi_idivmod) {
        i32 x = (i32)c.r[0], y = (i32)c.r[1];
        if (y == 0) { c.r[0] = 0; c.r[1] = 0; }
        else if (y == -1) { c.r[0] = 0u - (u32)x; c.r[1] = 0; }
        else { c.r[0] = (u32)(x / y); c.r[1] = (u32)(x % y); }
    });
    RAW(__aeabi_uidivmod) {
        u32 x = c.r[0], y = c.r[1];
        c.r[0] = y ? x / y : 0;
        c.r[1] = y ? x % y : 0;
    });
    RAW(__aeabi_ldivmod) {
        i64 x = (i64)r01(c), y = (i64)r23(c);
        i64 q = 0, r = 0;
        if (y == -1) q = (i64)(0ull - (u64)x);
        else if (y != 0) { q = x / y; r = x % y; }
        c.r[0] = (u32)q; c.r[1] = (u32)((u64)q >> 32); c.r[2] = (u32)r; c.r[3] = (u32)((u64)r >> 32);
    });
    RAW(__aeabi_uldivmod) {
        u64 x = r01(c), y = r23(c);
        u64 q = y ? x / y : 0, r = y ? x % y : 0;
        c.r[0] = (u32)q; c.r[1] = (u32)(q >> 32); c.r[2] = (u32)r; c.r[3] = (u32)(r >> 32);
    });
    reg("__divdi3", +[](long long x, long long y) -> long long { return (y == 0) ? 0 : (y == -1 ? (long long)(0ull - (u64)x) : x / y); });
    reg("__moddi3", +[](long long x, long long y) -> long long { return (y == 0 || y == -1) ? 0 : x % y; });
    reg("__udivdi3", +[](unsigned long long x, unsigned long long y) -> unsigned long long { return y ? x / y : 0; });
    reg("__umoddi3", +[](unsigned long long x, unsigned long long y) -> unsigned long long { return y ? x % y : 0; });
    RAW(__aeabi_lmul) { c.set_ret64(r01(c) * r23(c)); });
    RAW(__aeabi_llsl) { c.set_ret64(c.r[2] >= 64 ? 0 : r01(c) << c.r[2]); });
    RAW(__aeabi_llsr) { c.set_ret64(c.r[2] >= 64 ? 0 : r01(c) >> c.r[2]); });
    RAW(__aeabi_lasr) { c.set_ret64((u64)((i64)r01(c) >> (c.r[2] >= 64 ? 63 : c.r[2]))); });

    // soft-float helpers (libraries built for plain "armeabi")
#define DOP(name, expr) RAW(name) { double x = as_d(r01(c)), y = as_d(r23(c)); (void)y; c.set_ret64(bits((double)(expr))); });
#define FOP(name, expr) RAW(name) { float x = as_f(c.r[0]), y = as_f(c.r[1]); (void)y; c.set_ret32(bits((float)(expr))); });
#define DCMP(name, expr) RAW(name) { double x = as_d(r01(c)), y = as_d(r23(c)); c.set_ret32((expr) ? 1 : 0); });
#define FCMP(name, expr) RAW(name) { float x = as_f(c.r[0]), y = as_f(c.r[1]); c.set_ret32((expr) ? 1 : 0); });
    DOP(__aeabi_dadd, x + y) DOP(__aeabi_dsub, x - y) DOP(__aeabi_drsub, y - x) DOP(__aeabi_dmul, x * y) DOP(__aeabi_ddiv, x / y)
    FOP(__aeabi_fadd, x + y) FOP(__aeabi_fsub, x - y) FOP(__aeabi_frsub, y - x) FOP(__aeabi_fmul, x * y) FOP(__aeabi_fdiv, x / y)
    DCMP(__aeabi_dcmpeq, x == y) DCMP(__aeabi_dcmplt, x < y) DCMP(__aeabi_dcmple, x <= y) DCMP(__aeabi_dcmpge, x >= y)
    DCMP(__aeabi_dcmpgt, x > y) DCMP(__aeabi_dcmpun, std::isnan(x) || std::isnan(y))
    FCMP(__aeabi_fcmpeq, x == y) FCMP(__aeabi_fcmplt, x < y) FCMP(__aeabi_fcmple, x <= y) FCMP(__aeabi_fcmpge, x >= y)
    FCMP(__aeabi_fcmpgt, x > y) FCMP(__aeabi_fcmpun, std::isnan(x) || std::isnan(y))
#undef DOP
#undef FOP
#undef DCMP
#undef FCMP
    RAW(__aeabi_f2d) { c.set_ret64(bits((double)as_f(c.r[0]))); });
    RAW(__aeabi_d2f) { c.set_ret32(bits((float)as_d(r01(c)))); });
    RAW(__aeabi_i2d) { c.set_ret64(bits((double)(i32)c.r[0])); });
    RAW(__aeabi_ui2d) { c.set_ret64(bits((double)c.r[0])); });
    RAW(__aeabi_i2f) { c.set_ret32(bits((float)(i32)c.r[0])); });
    RAW(__aeabi_ui2f) { c.set_ret32(bits((float)c.r[0])); });
    RAW(__aeabi_l2d) { c.set_ret64(bits((double)(i64)r01(c))); });
    RAW(__aeabi_ul2d) { c.set_ret64(bits((double)r01(c))); });
    RAW(__aeabi_l2f) { c.set_ret32(bits((float)(i64)r01(c))); });
    RAW(__aeabi_ul2f) { c.set_ret32(bits((float)r01(c))); });
    auto sat = [](double d, double lo, double hi) { return std::isnan(d) ? 0.0 : d < lo ? lo : d > hi ? hi : d; };
    static auto s_sat = +sat;
    RAW(__aeabi_d2iz) { c.set_ret32((u32)(i32)s_sat(as_d(r01(c)), -2147483648.0, 2147483647.0)); });
    RAW(__aeabi_d2uiz) { c.set_ret32((u32)s_sat(as_d(r01(c)), 0.0, 4294967295.0)); });
    RAW(__aeabi_f2iz) { c.set_ret32((u32)(i32)s_sat(as_f(c.r[0]), -2147483648.0, 2147483647.0)); });
    RAW(__aeabi_f2uiz) { c.set_ret32((u32)s_sat(as_f(c.r[0]), 0.0, 4294967295.0)); });
    RAW(__aeabi_d2lz) { c.set_ret64((u64)(i64)s_sat(as_d(r01(c)), -9223372036854775808.0, 9223372036854774784.0)); });
    RAW(__aeabi_d2ulz) { c.set_ret64((u64)s_sat(as_d(r01(c)), 0.0, 18446744073709549568.0)); });
    RAW(__aeabi_f2lz) { c.set_ret64((u64)(i64)s_sat(as_f(c.r[0]), -9223372036854775808.0, 9223372036854774784.0)); });
    RAW(__aeabi_f2ulz) { c.set_ret64((u64)s_sat(as_f(c.r[0]), 0.0, 18446744073709549568.0)); });
}

}  // namespace a64b
