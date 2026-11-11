// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu.h"

#include <unicorn/unicorn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "syscalls.h"

namespace a64b {

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
};

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
    g_traps[idx] = TrapEntry{fn, ud, name ? strdup(name) : "?"};
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
std::atomic<int> g_gil_waiters{0};
}  // namespace

void gil_acquire(Cpu* c) {
    std::unique_lock<std::mutex> lk(g_gil_mu);
    u64 my = g_next_ticket++;
    if (g_serving != my) {
        g_gil_waiters.fetch_add(1);
        while (g_serving != my) {
            if (g_gil_cv.wait_for(lk, std::chrono::milliseconds(2)) == std::cv_status::timeout) {
                if (g_serving != my && g_gil_owner) uc_emu_stop(g_gil_owner);
            }
        }
        g_gil_waiters.fetch_sub(1);
    }
    g_gil_owner = c ? c->uc : nullptr;
}

void gil_release() {
    {
        std::lock_guard<std::mutex> lk(g_gil_mu);
        g_gil_owner = nullptr;
        g_serving++;
    }
    g_gil_cv.notify_all();
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

void cpu_dump_state(Cpu& c, const char* why) {
    LOGE("=== %s ===", why);
    LOGE(" registers at the last guest->host call:");
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
}

void guest_crash(Cpu& c, const char* why) {
    cpu_dump_state(c, strfmt("guest crash: %s", why).c_str());
    fatal("guest code crashed (%s)", why);
}

void Cpu::run(u32 pc) {
    depth++;
    for (;;) {
        if (epoch != mem_code_epoch()) {
            epoch = mem_code_epoch();
            uc_ctl_flush_tb(uc);
        }
        trap_pending = false;
        bad_excp = 0;
        gil_acquire(this);
        uc_err e = uc_emu_start(uc, pc, 0, 0, 0);
        gil_release();
        load_regs();
        if (e != UC_ERR_OK) guest_crash(*this, uc_strerror(e));
        if (!trap_pending) {
            if (bad_excp) guest_crash(*this, strfmt("unexpected CPU exception %u", bad_excp & 0xFF).c_str());
            if (r[15] == 0) guest_crash(*this, "jump to NULL");
            // Asynchronous stop (another thread wants the GIL): just resume.
            pc = r[15] | ((cpsr >> 5) & 1);
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
            if (!g_trap_called[idx].exchange(1, std::memory_order_relaxed)) LOGI("first call: %s", t.name);
            t.fn(*this, t.ud);
            store_regs(regs_dirty);
            pc = pc_override ? r[15] : lr;
        } else {
            // A raw Linux system call made by guest code.
            regs_dirty = false;
            u32 next = r[15] | (thumb ? 1 : 0);
            r[0] = guest_syscall(*this, r[7]);
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
