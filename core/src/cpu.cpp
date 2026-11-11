// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu.h"

#include <unicorn/unicorn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include "syscalls.h"

namespace a64b {

std::atomic<u64> g_guest_host_calls{0};
std::atomic<u64> g_frames{0};
std::atomic<int> g_guest_threads{0};
std::atomic<u64> g_gil_switches{0};

// ---------------------------------------------------------------------------
// Trap stubs.  Every slot is "svc #0; bx lr" (ARM mode). The slot index selects
// the host handler. All slots are written once at start-up so translated code
// never becomes stale.
// ---------------------------------------------------------------------------
namespace {

constexpr u32 kMaxTraps = 32768;
constexpr u32 kStubSize = 8;

struct TrapEntry {
    TrapFn fn;
    void* ud;
    const char* name;
    bool fast;  // never waits for another guest thread: runs without dropping the GIL
};

// Host functions that cannot wait for another guest thread. Everything else drops the
// GIL while it runs, so that a blocking call never stalls the other guest threads.
bool is_fast_trap(const char* name) {
    std::string_view s(name);
    auto starts = [&](const char* p) { return s.rfind(p, 0) == 0; };
    if (starts("JNI:") || starts("JavaVM:")) return false;          // Java may wait on other threads
    if (starts("gl")) return s != "glFinish";
    if (starts("__aeabi_") || starts("str") || starts("mem") || starts("wcs") || starts("wmem") || starts("is") ||
        starts("to") || starts("__is") || starts("__fpclassify") || starts("__signbit") || starts("__str") ||
        starts("__mem") || starts("pthread_mutexattr_") || starts("pthread_condattr_") || starts("pthread_attr_") ||
        starts("pthread_rwlockattr_"))
        return true;
    static const char* const kFast[] = {
        "malloc", "calloc", "realloc", "free", "memalign", "posix_memalign", "aligned_alloc", "valloc",
        "malloc_usable_size", "dlmalloc_usable_size", "_Znwj", "_Znaj", "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t",
        "_ZdlPv", "_ZdaPv", "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t", "_ZdlPvj", "_ZdaPvj", "bcmp", "bcopy",
        "bzero", "index", "rindex", "sprintf", "snprintf", "vsprintf", "vsnprintf", "__sprintf_chk", "__snprintf_chk",
        "__vsprintf_chk", "__vsnprintf_chk", "asprintf", "vasprintf", "sscanf", "vsscanf", "swprintf", "vswprintf",
        "atoi", "atol", "atoll", "atof", "abs", "labs", "llabs", "div", "ldiv", "lldiv", "rand", "srand", "random",
        "srandom", "rand_r", "lrand48", "mrand48", "drand48", "srand48", "arc4random", "arc4random_uniform",
        "clock_gettime", "gettimeofday", "time", "clock", "__errno", "getpid", "gettid", "pthread_self",
        "pthread_equal", "pthread_getspecific", "pthread_setspecific", "pthread_mutex_lock", "pthread_mutex_unlock",
        "pthread_mutex_trylock", "pthread_mutex_init", "pthread_mutex_destroy", "pthread_cond_signal",
        "pthread_cond_broadcast", "pthread_cond_init", "pthread_cond_destroy", "pthread_rwlock_unlock",
        "pthread_rwlock_tryrdlock", "pthread_rwlock_trywrlock", "pthread_rwlock_init", "pthread_rwlock_destroy",
        "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "sem_post", "sem_trywait", "sem_getvalue", "sem_wait",
        "__cxa_guard_release", "__cxa_guard_abort", "__cxa_atexit", "__aeabi_atexit", "qsort", "bsearch",
        "localtime", "gmtime", "localtime_r", "gmtime_r", "mktime", "difftime", "timegm", "mbstowcs", "wcstombs",
        "mbtowc", "wctomb", "mblen", "btowc", "wctob", "mbrtowc", "wcrtomb", "mbrlen", "mbsinit", "wctype",
        "iswctype", "crc32", "adler32", "crc32_combine", "adler32_combine", "compress", "compress2", "compressBound",
        "uncompress", "inflate", "inflateInit_", "inflateInit2_", "inflateEnd", "inflateReset", "inflateReset2",
        "deflate", "deflateInit_", "deflateInit2_", "deflateEnd", "deflateReset", "deflateBound", "deflateParams",
        "zlibVersion", "zError", "eglGetError", "eglGetCurrentContext", "eglGetCurrentDisplay",
        "eglGetCurrentSurface", "eglGetProcAddress", "AAsset_read", "AAsset_seek", "AAsset_seek64",
        "AAsset_getLength", "AAsset_getLength64", "AAsset_getRemainingLength", "AAsset_getRemainingLength64",
        "__android_log_print", "__android_log_write", "__android_log_vprint", "__android_log_buf_write",
        "__android_log_buf_print", "__android_log_is_loggable", "setjmp", "_setjmp", "sigsetjmp", "longjmp",
        "_longjmp", "siglongjmp", "sinf", "cosf", "sqrtf", "floorf", "ceilf", "powf", "atan2f", "fmodf", "expf",
        "logf", "tanf", "fabsf", "sin", "cos", "sqrt", "floor", "ceil", "pow", "atan2", "fmod", "exp", "log", "tan",
        "fabs", "exp2f", "log10f", "roundf", "lroundf", "acosf", "asinf", "atanf", "fminf", "fmaxf", "ldexpf",
        "frexpf", "modff", "sincosf", "hypotf", "log2f", "truncf", "round", "lround", "trunc", "acos", "asin",
        "atan", "fmin", "fmax", "ldexp", "frexp", "modf", "sincos", "hypot", "log2", "log10", "exp2"};
    for (const char* f : kFast)
        if (s == f) return true;
    return false;
}

u32 g_trap_base = 0;
TrapEntry* g_traps = nullptr;
std::atomic<u32> g_trap_count{0};
u32 g_return_trap = 0;
std::atomic<u8>* g_trap_called = nullptr;

void return_trap(Cpu&, void*) {}

}  // namespace

u32 trap_create(const char* name, TrapFn fn, void* ud) {
    u32 idx = g_trap_count.fetch_add(1);
    if (idx >= kMaxTraps) fatal("out of trap slots");
    g_traps[idx] = TrapEntry{fn, ud, name ? strdup(name) : "?", name && is_fast_trap(name)};
    return g_trap_base + idx * kStubSize;
}

const char* trap_name_by_index(u32 idx) { return idx < g_trap_count.load() ? g_traps[idx].name : nullptr; }

const char* trap_name_at(u32 addr) {
    if (addr < g_trap_base || addr >= g_trap_base + kMaxTraps * kStubSize) return nullptr;
    u32 idx = (addr - g_trap_base) / kStubSize;
    return idx < g_trap_count.load() ? g_traps[idx].name : nullptr;
}

bool cpu_global_init() {
    if (g_trap_base) return true;
    if (!mem_init()) return false;
    g_traps = new TrapEntry[kMaxTraps]();
    g_trap_called = new std::atomic<u8>[kMaxTraps]();
    u32 base = page_alloc(kMaxTraps * kStubSize, true);
    if (!base) return false;
    u32* p = g2h<u32>(base);
    for (u32 i = 0; i < kMaxTraps; i++) {
        p[i * 2] = 0xEF000000;      // svc #0
        p[i * 2 + 1] = 0xE12FFF1E;  // bx lr
    }
    g_trap_base = base;
    g_return_trap = trap_create("<return>", return_trap, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// GIL (FIFO ticket lock; a waiter pokes the running engine so it yields).
// ---------------------------------------------------------------------------
namespace {
std::mutex g_gil_mu;
std::condition_variable g_gil_cv;
u64 g_next_ticket = 0, g_serving = 0;
uc_struct* g_gil_owner = nullptr;
std::atomic<bool> g_yield_request{false};
thread_local int t_gil_depth = 0;

void gil_lock_raw(Cpu* c) {
    std::unique_lock<std::mutex> lk(g_gil_mu);
    u64 my = g_next_ticket++;
    while (g_serving != my) {
        if (my == g_serving + 1) {
            // Next in line: after one time slice, ask the running thread to yield.
            if (g_gil_cv.wait_for(lk, std::chrono::milliseconds(3)) == std::cv_status::timeout && g_serving != my) {
                g_yield_request.store(true, std::memory_order_relaxed);
                if (g_gil_owner) uc_emu_stop(g_gil_owner);
            }
        } else {
            g_gil_cv.wait(lk);
        }
    }
    g_gil_owner = c ? c->uc : nullptr;
    g_gil_switches.fetch_add(1, std::memory_order_relaxed);
}

void gil_unlock_raw() {
    {
        std::lock_guard<std::mutex> lk(g_gil_mu);
        g_gil_owner = nullptr;
        g_serving++;
    }
    g_gil_cv.notify_all();
}
}  // namespace

// The GIL is re-entrant per thread: host functions such as qsort call back into guest code.
void gil_acquire(Cpu* c) {
    if (t_gil_depth++ == 0) gil_lock_raw(c);
}

void gil_release() {
    if (--t_gil_depth == 0) gil_unlock_raw();
}

int gil_release_all() {
    int d = t_gil_depth;
    if (d) {
        t_gil_depth = 0;
        gil_unlock_raw();
    }
    return d;
}

void gil_restore(int depth, Cpu* c) {
    if (depth) {
        gil_lock_raw(c);
        t_gil_depth = depth;
    }
}

void gil_maybe_yield(Cpu* c) {
    if (!g_yield_request.load(std::memory_order_relaxed)) return;
    g_yield_request.store(false, std::memory_order_relaxed);
    int d = gil_release_all();
    gil_restore(d, c);
}

// ---------------------------------------------------------------------------
// Cpu
// ---------------------------------------------------------------------------
namespace {

const int kAllRegs[17] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3,  UC_ARM_REG_R4, UC_ARM_REG_R5,
                          UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,  UC_ARM_REG_R9,  UC_ARM_REG_R10, UC_ARM_REG_R11,
                          UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR};

void intr_hook(uc_engine* uc, uint32_t intno, void* user) {
    Cpu* c = static_cast<Cpu*>(user);
    if (intno == 2 /* EXCP_SWI */) {
        c->trap_pending = true;
        uc_emu_stop(uc);
    } else {
        // Leave trap_pending false; the run loop reports the failure.
        c->bad_excp = intno | 0x100;
        uc_emu_stop(uc);
    }
}

bool mem_fault_hook(uc_engine*, uc_mem_type type, uint64_t addr, int, int64_t, void* user) {
    Cpu* c = static_cast<Cpu*>(user);
    c->fault_addr = addr;
    c->fault_type = (int)type;
    return false;
}

constexpr u32 kDefaultStack = 2 * 1024 * 1024;

}  // namespace

struct CpuHolder {
    Cpu* cpu = nullptr;
    ~CpuHolder() { delete cpu; }
    Cpu* get() {
        if (!cpu) cpu = new Cpu();
        return cpu;
    }
};
static thread_local CpuHolder t_cpu;

Cpu& Cpu::current() { return *t_cpu.get(); }
Cpu* Cpu::current_or_null() { return t_cpu.cpu; }

Cpu::Cpu() {
    if (!cpu_global_init()) fatal("bridge initialisation failed");
    uc_err e = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (e != UC_ERR_OK) fatal("uc_open: %s", uc_strerror(e));
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_MAX);
    // Unicorn reserves 1 GiB of RWX memory per engine by default; there is one engine per guest thread.
    uc_ctl_set_tcg_buffer_size(uc, 256u << 20);
    e = uc_mem_map_ptr(uc, mem_arena_base(), mem_arena_size(), UC_PROT_ALL, g2h(mem_arena_base()));
    if (e != UC_ERR_OK) fatal("uc_mem_map_ptr: %s", uc_strerror(e));
    uc_hook hh;
    e = uc_hook_add(uc, &hh, UC_HOOK_INTR, (void*)intr_hook, this, 1, 0);
    if (e != UC_ERR_OK) fatal("uc_hook_add: %s", uc_strerror(e));
    e = uc_hook_add(uc, &hh, UC_HOOK_MEM_INVALID, (void*)mem_fault_hook, this, 1, 0);
    if (e != UC_ERR_OK) fatal("uc_hook_add: %s", uc_strerror(e));

    // Enable VFP/NEON: CPACR.cp10/cp11 = full access, FPEXC.EN = 1.
    u32 v = 0x00F00000;
    uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &v);
    v = 0x40000000;
    uc_reg_write(uc, UC_ARM_REG_FPEXC, &v);

    stack_size = kDefaultStack;
    stack_base = page_alloc(stack_size);
    tls_block = g_calloc(1, 512);
    errno_addr = tls_block + 256;
    if (!stack_base || !tls_block) fatal("cannot allocate guest thread state");
    uc_reg_write(uc, UC_ARM_REG_C13_C0_3, &tls_block);
    u32 sp = stack_base + stack_size - 64;
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    r[13] = sp;
    epoch = mem_code_epoch();
}

Cpu::~Cpu() {
    if (uc) uc_close(uc);
    if (stack_base) page_free(stack_base, stack_size);
    if (tls_block) g_free(tls_block);
}

void Cpu::sync_errno() { *g2h<i32>(errno_addr) = errno; }

void Cpu::load_regs() {
    void* ptrs[17];
    for (int i = 0; i < 16; i++) ptrs[i] = &r[i];
    ptrs[16] = &cpsr;
    uc_reg_read_batch(uc, const_cast<int*>(kAllRegs), ptrs, 17);
}

void Cpu::store_regs(bool all) {
    // PC and CPSR are set through uc_emu_start (begin address carries the Thumb bit).
    void* ptrs[15];
    int n = all ? 15 : 4;
    for (int i = 0; i < n; i++) ptrs[i] = &r[i];
    uc_reg_write_batch(uc, const_cast<int*>(kAllRegs), (void* const*)ptrs, n);
}

void cpu_dump_state(Cpu& c, const char* why, bool at_fault) {
    LOGE("=== %s ===", why);
    if (at_fault && c.fault_type) {
        const char* kind = "invalid access";
        switch (c.fault_type) {
            case UC_MEM_READ_UNMAPPED: kind = "read of unmapped memory"; break;
            case UC_MEM_WRITE_UNMAPPED: kind = "write to unmapped memory"; break;
            case UC_MEM_FETCH_UNMAPPED: kind = "execution of unmapped memory"; break;
            case UC_MEM_WRITE_PROT: kind = "write to protected memory"; break;
            case UC_MEM_READ_PROT: kind = "read of protected memory"; break;
            case UC_MEM_FETCH_PROT: kind = "execution of protected memory"; break;
            default: break;
        }
        LOGE(" %s at address 0x%08llx%s", kind,
             (unsigned long long)c.fault_addr, c.fault_addr < 0x10000 ? " (NULL pointer)" : "");
    }
    LOGE(" registers %s:", at_fault ? "at the fault" : "at the last guest->host call");
    LOGE(" r0=%08x r1=%08x r2=%08x r3=%08x", c.r[0], c.r[1], c.r[2], c.r[3]);
    LOGE(" r4=%08x r5=%08x r6=%08x r7=%08x", c.r[4], c.r[5], c.r[6], c.r[7]);
    LOGE(" r8=%08x r9=%08x r10=%08x r11=%08x", c.r[8], c.r[9], c.r[10], c.r[11]);
    LOGE(" ip=%08x sp=%08x lr=%08x pc=%08x cpsr=%08x", c.r[12], c.r[13], c.r[14], c.r[15], c.cpsr);
    LOGE(" pc: %s", describe_address(c.r[15]).c_str());
    LOGE(" lr: %s", describe_address(c.r[14]).c_str());
    std::string tr;
    for (u32 i = 0; i < 32; i++) {
        u32 v = c.trace[(c.trace_pos + i) % 32];
        if (!v) continue;
        const char* n = trap_name_by_index(v - 1);
        tr += n ? n : "?";
        tr += " ";
    }
    LOGE(" last host calls (oldest first): %s", tr.c_str());
    // Heuristic backtrace: code addresses found on the guest stack.
    u32 sp = c.r[13];
    if (sp >= c.stack_base && sp < c.stack_base + c.stack_size) {
        int found = 0;
        for (u32 a = sp; a + 4 <= c.stack_base + c.stack_size && a < sp + 16384 && found < 20; a += 4) {
            u32 v = *g2h<u32>(a);
            std::string d = describe_address(v & ~1u);
            if (d.find(".so+") == std::string::npos) continue;
            LOGE("  stack sp+0x%04x: %s", a - sp, d.c_str());
            found++;
        }
    }
    c.crash_reported = true;
}

void guest_crash(Cpu& c, const char* why) {
    cpu_dump_state(c, strfmt("guest crash: %s", why).c_str(), true);
    fatal("guest code crashed (%s)", why);
}

void Cpu::run(u32 pc) {
    depth++;
    struct Hold {
        Cpu* c;
        explicit Hold(Cpu* cpu) : c(cpu) { gil_acquire(c); }
        ~Hold() { gil_release(); }
    } hold(this);
    for (;;) {
        if (epoch != mem_code_epoch()) {
            epoch = mem_code_epoch();
            uc_ctl_flush_tb(uc);
        }
        trap_pending = false;
        bad_excp = 0;
        fault_addr = 0;
        fault_type = 0;
        uc_err e = uc_emu_start(uc, pc, 0, 0, 0);
        load_regs();
        if (e != UC_ERR_OK) guest_crash(*this, uc_strerror(e));
        if (!trap_pending) {
            if (bad_excp) guest_crash(*this, strfmt("unexpected CPU exception %u", bad_excp & 0xFF).c_str());
            if (r[15] == 0) guest_crash(*this, "jump to NULL");
            // Asynchronous stop: another thread wants the GIL.
            pc = r[15] | ((cpsr >> 5) & 1);
            g_yield_request.store(true, std::memory_order_relaxed);
            gil_maybe_yield(this);
            continue;
        }
        // PC points just after the SVC instruction.
        bool thumb = (cpsr >> 5) & 1;
        u32 svc_addr = r[15] - (thumb ? 2 : 4);
        if (svc_addr >= g_trap_base && svc_addr < g_trap_base + kMaxTraps * kStubSize && !thumb) {
            u32 idx = (svc_addr - g_trap_base) / kStubSize;
            if (svc_addr == g_return_trap) break;
            TrapEntry& t = g_traps[idx];
            if (!t.fn) guest_crash(*this, "call through an unassigned trap slot");
            regs_dirty = false;
            pc_override = false;
            u32 lr = r[14];
            trace[trace_pos++ % 32] = idx + 1;
            g_guest_host_calls.fetch_add(1, std::memory_order_relaxed);
            if (!g_trap_called[idx].exchange(1, std::memory_order_relaxed)) LOGI("first call: %s", t.name);
            if (t.fast) {
                t.fn(*this, t.ud);
            } else {
                GilUnlock unlocked(this);
                t.fn(*this, t.ud);
            }
            store_regs(regs_dirty);
            pc = pc_override ? r[15] : lr;
            gil_maybe_yield(this);
        } else {
            // A raw Linux system call made by guest code.
            regs_dirty = false;
            u32 next = r[15] | (thumb ? 1 : 0);
            {
                GilUnlock unlocked(this);
                r[0] = guest_syscall(*this, r[7]);
            }
            store_regs(regs_dirty);
            pc = next;
        }
    }
    depth--;
}

u64 Cpu::call(u32 fn, const u32* words, size_t nwords) {
    if (!fn) fatal("attempt to call a NULL guest function");
    // Preserve the interrupted context (we may be nested inside a trap handler).
    u32 saved[16];
    u32 saved_cpsr = cpsr;
    bool saved_dirty = regs_dirty, saved_override = pc_override;
    if (depth > 0) memcpy(saved, r, sizeof(saved));

    u32 sp = (depth > 0 ? r[13] : stack_base + stack_size - 64);
    size_t nstack = nwords > 4 ? nwords - 4 : 0;
    sp -= (u32)(nstack * 4);
    sp &= ~7u;
    if (sp < stack_base + 4096) fatal("guest stack overflow");
    for (size_t i = 0; i < nstack; i++) g2h<u32>(sp)[i] = words[4 + i];

    u32 regs[16] = {};
    if (depth > 0) memcpy(regs, r, sizeof(regs));
    for (size_t i = 0; i < 4; i++) regs[i] = i < nwords ? words[i] : 0;
    regs[13] = sp;
    regs[14] = g_return_trap;
    memcpy(r, regs, sizeof(regs));
    store_regs(true);

    run(fn);

    u64 ret = (u64)r[0] | ((u64)r[1] << 32);
    if (depth > 0) {
        memcpy(r, saved, sizeof(saved));
        cpsr = saved_cpsr;
        store_regs(true);
    }
    regs_dirty = saved_dirty;
    pc_override = saved_override;
    return ret;
}

}  // namespace a64b
