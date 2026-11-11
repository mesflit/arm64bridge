// CPU backend: dynarmic translates the guest's ARMv7 code to host machine code.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <array>
#include <atomic>
#include <mutex>

#include "syscalls.h"

namespace a64b {

std::atomic<u64> g_guest_host_calls{0};
std::atomic<u64> g_frames{0};
std::atomic<int> g_guest_threads{0};
std::atomic<int> g_render_tid{0};
std::atomic<int> g_frame_limiter{0};

// ---------------------------------------------------------------------------
// Trap stubs. Slot i is "svc #(i+1); bx lr" (ARM mode): the SVC number selects the
// host function directly. "svc #0" is left for raw system calls made by guest code.
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
u32 g_return_trap = 0, g_return_idx = 0;
std::atomic<u8>* g_trap_called = nullptr;

void return_trap(Cpu&, void*) {}

// Identity page table: entry = host address of the guest page (guest address == host address).
using PageTable = std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
PageTable* g_page_table = nullptr;

// LDREX/STREX across guest threads.
constexpr size_t kMaxProcessors = 1024;
Dynarmic::ExclusiveMonitor* g_monitor = nullptr;
std::mutex g_pid_mu;
std::vector<size_t> g_free_pids;

size_t take_pid() {
    std::lock_guard<std::mutex> lk(g_pid_mu);
    if (g_free_pids.empty()) fatal("too many guest threads (limit %zu JIT instances)", kMaxProcessors);
    size_t p = g_free_pids.back();
    g_free_pids.pop_back();
    return p;
}
void give_pid(size_t p) {
    std::lock_guard<std::mutex> lk(g_pid_mu);
    g_free_pids.push_back(p);
}

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
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        if (!mem_init()) return;
        g_traps = new TrapEntry[kMaxTraps]();
        g_trap_called = new std::atomic<u8>[kMaxTraps]();
        u32 base = page_alloc(kMaxTraps * kStubSize, true);
        if (!base) return;
        u32* p = g2h<u32>(base);
        for (u32 i = 0; i < kMaxTraps; i++) {
            p[i * 2] = 0xEF000000u | (i + 1);  // svc #(i+1)
            p[i * 2 + 1] = 0xE12FFF1E;         // bx lr
        }
        g_page_table = new PageTable();
        g_page_table->fill(nullptr);
        for (u64 a = mem_arena_base(); a < (u64)mem_arena_base() + mem_arena_size(); a += 4096)
            (*g_page_table)[a >> 12] = reinterpret_cast<std::uint8_t*>(static_cast<uintptr_t>(a));
        g_monitor = new Dynarmic::ExclusiveMonitor(kMaxProcessors);
        for (size_t i = kMaxProcessors; i-- > 0;) g_free_pids.push_back(i);
        g_trap_base = base;
        g_return_trap = trap_create("<return>", return_trap, nullptr);
        g_return_idx = (g_return_trap - g_trap_base) / kStubSize;
        ok = true;
    });
    return ok;
}

// ---------------------------------------------------------------------------
// One JIT instance. A thread has one per nesting level: a host function called by
// guest code may call back into guest code (qsort comparators, Java calling a
// native method during a JNI call), and a dynarmic JIT cannot be re-entered.
// ---------------------------------------------------------------------------
namespace {

// CP15: only the user-space thread ID registers exist.
class Cp15 final : public Dynarmic::A32::Coprocessor {
  public:
    explicit Cp15(u32* tls) : tls_(tls) {}
    using CoprocReg = Dynarmic::A32::CoprocReg;
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && (opc2 == 2 || opc2 == 3)) return tls_;
        if (!two && CRn == CoprocReg::C7) return &scratch_;  // cache maintenance / barriers
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && (opc2 == 2 || opc2 == 3)) return tls_;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }

  private:
    u32* tls_;
    u32 scratch_ = 0;
};

}  // namespace

struct CpuLevel final : Dynarmic::A32::UserCallbacks {
    Cpu& cpu;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    size_t pid;
    bool returned = false;

    CpuLevel(Cpu& c, size_t code_cache) : cpu(c), pid(take_pid()) {
        Dynarmic::A32::UserConfig cfg{};
        cfg.callbacks = this;
        cfg.processor_id = pid;
        cfg.global_monitor = g_monitor;
        cfg.page_table = g_page_table;
        cfg.coprocessors[15] = std::make_shared<Cp15>(&c.tls_block);
        cfg.enable_cycle_counting = false;
        cfg.code_cache_size = code_cache;
        jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
    }
    ~CpuLevel() override {
        jit.reset();
        give_pid(pid);
    }

    [[noreturn]] void bad_access(u32 vaddr, int type) {
        cpu.fault_addr = vaddr;
        cpu.fault_type = type;
        guest_crash(cpu, type == 2 ? "invalid memory write" : "invalid memory read");
    }
    template <class T>
    T rd(u32 a) {
        if (!mem_contains(a) || !mem_contains((u64)a + sizeof(T) - 1)) bad_access(a, 1);
        T v;
        memcpy(&v, g2h(a), sizeof(T));
        return v;
    }
    template <class T>
    void wr(u32 a, T v) {
        if (!mem_contains(a) || !mem_contains((u64)a + sizeof(T) - 1)) bad_access(a, 2);
        memcpy(g2h(a), &v, sizeof(T));
    }
    template <class T>
    bool cas(u32 a, T value, T expected) {
        if (!mem_contains(a)) bad_access(a, 2);
        return __atomic_compare_exchange_n(g2h<T>(a), &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t a) override {
        if (!mem_contains(a)) return std::nullopt;
        return *g2h<u32>(a);
    }
    // Only reached for addresses outside the page table (i.e. outside guest memory).
    std::uint8_t MemoryRead8(std::uint32_t a) override { return rd<u8>(a); }
    std::uint16_t MemoryRead16(std::uint32_t a) override { return rd<u16>(a); }
    std::uint32_t MemoryRead32(std::uint32_t a) override { return rd<u32>(a); }
    std::uint64_t MemoryRead64(std::uint32_t a) override { return rd<u64>(a); }
    void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { wr(a, v); }
    void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { wr(a, v); }
    void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { wr(a, v); }
    void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { wr(a, v); }
    bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }

    void InterpreterFallback(std::uint32_t pc, size_t) override {
        cpu.r[15] = pc;
        guest_crash(cpu, "instruction not supported by the translator");
    }
    void CallSVC(std::uint32_t swi) override { cpu.on_svc(swi); }
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override {
        cpu.r[15] = pc;
        guest_crash(cpu, strfmt("CPU exception %d (undefined or unpredictable instruction)", (int)e).c_str());
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1u << 30; }
};

// ---------------------------------------------------------------------------
// Cpu
// ---------------------------------------------------------------------------
namespace {
constexpr u32 kDefaultStack = 2 * 1024 * 1024;
}

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
    stack_size = kDefaultStack;
    stack_base = page_alloc(stack_size);
    tls_block = g_calloc(1, 512);
    errno_addr = tls_block + 256;
    if (!stack_base || !tls_block) fatal("cannot allocate guest thread state");
    idle_regs[13] = stack_base + stack_size - 64;
    epoch = mem_code_epoch();
}

Cpu::~Cpu() {
    for (CpuLevel* l : levels) delete l;
    if (stack_base) page_free(stack_base, stack_size);
    // tls_block may have been replaced by the guest (set_tls); the original allocation is small, leak it.
}

void Cpu::sync_errno() { *g2h<i32>(errno_addr) = errno; }

u32* Cpu::ext_regs() {
    if (depth <= 0) return nullptr;
    return levels[depth - 1]->jit->ExtRegs().data();
}

void cpu_dump_state(Cpu& c, const char* why, bool at_fault) {
    LOGE("=== %s ===", why);
    if (at_fault && c.fault_type)
        LOGE(" %s at address 0x%08llx%s", c.fault_type == 2 ? "write to unmapped memory" : "read of unmapped memory",
             (unsigned long long)c.fault_addr, c.fault_addr < 0x10000 ? " (NULL pointer)" : "");
    LOGE(" registers %s:", at_fault ? "at the fault" : "at the last guest->host call");
    LOGE(" r0=%08x r1=%08x r2=%08x r3=%08x", c.r[0], c.r[1], c.r[2], c.r[3]);
    LOGE(" r4=%08x r5=%08x r6=%08x r7=%08x", c.r[4], c.r[5], c.r[6], c.r[7]);
    LOGE(" r8=%08x r9=%08x r10=%08x r11=%08x", c.r[8], c.r[9], c.r[10], c.r[11]);
    LOGE(" ip=%08x sp=%08x lr=%08x pc=%08x", c.r[12], c.r[13], c.r[14], c.r[15]);
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
    // Text the crashing code was working with: strings reachable from the stack and registers
    // (error messages, file names...). One level of indirection covers string objects.
    {
        std::vector<std::string> seen;
        auto try_string = [&](u32 p) {
            if (!mem_is_mapped(p, 8)) return;
            const char* s = g2h<char>(p);
            size_t n = 0;
            while (n < 300 && mem_is_mapped(p + (u32)n) && s[n] >= 0x20 && s[n] < 0x7f) n++;
            if (n < 6 || n >= 300 || !mem_is_mapped(p + (u32)n) || s[n] != 0) return;
            std::string str(s, n);
            for (auto& o : seen)
                if (o == str) return;
            if (seen.size() < 40) seen.push_back(str);
        };
        auto consider = [&](u32 v) {
            try_string(v);
            if ((v & 3) == 0 && mem_is_mapped(v, 16))
                for (int k = 0; k < 4; k++) try_string(g2h<u32>(v)[k]);
        };
        for (int i = 0; i < 13; i++) consider(c.r[i]);
        if (sp >= c.stack_base && sp < c.stack_base + c.stack_size)
            for (u32 a = sp; a + 4 <= c.stack_base + c.stack_size && a < sp + 8192; a += 4) consider(*g2h<u32>(a));
        for (auto& str : seen) LOGE("  text: %s", str.c_str());
    }
    c.crash_reported = true;
}

void guest_crash(Cpu& c, const char* why) {
    cpu_dump_state(c, strfmt("guest crash: %s", why).c_str(), true);
    fatal("guest code crashed (%s)", why);
}

// Runs inside translated code. Host functions read and write the guest registers in place.
void Cpu::on_svc(u32 swi) {
    CpuLevel* lvl = levels[depth - 1];
    if (swi == 0) {  // raw Linux system call
        r[0] = guest_syscall(*this, r[7]);
    } else {
        u32 idx = swi - 1;
        if (idx == g_return_idx) {
            lvl->returned = true;
            lvl->jit->HaltExecution();
            return;
        }
        if (idx >= g_trap_count.load(std::memory_order_acquire) || !g_traps[idx].fn) guest_crash(*this, "call through an unassigned trap slot");
        TrapEntry& t = g_traps[idx];
        trace[trace_pos++ % 32] = idx + 1;
        g_guest_host_calls.fetch_add(1, std::memory_order_relaxed);
        if (!g_trap_called[idx].load(std::memory_order_relaxed) && !g_trap_called[idx].exchange(1)) LOGI("first call: %s", t.name);
        pc_override = false;
        try {
            t.fn(*this, t.ud);
        } catch (const ThreadExit& e) {
            // C++ exceptions cannot unwind through translated code: leave the JIT first.
            exit_pending = true;
            exit_value = e.retval;
            lvl->jit->HaltExecution();
            return;
        }
        if (pc_override) {  // longjmp: r[15] holds the target with the Thumb bit
            u32 target = r[15];
            lvl->jit->SetCpsr((lvl->jit->Cpsr() & ~0x20u) | ((target & 1) << 5));
            r[15] = target & ~1u;
            pc_override = false;
        }
    }
    // Stale translations (library loaded, guest JIT flushed its cache): leave so the loop can clear them.
    if (epoch != mem_code_epoch()) lvl->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

u64 Cpu::call(u32 fn, const u32* words, size_t nwords) {
    if (!fn) fatal("attempt to call a NULL guest function");
    if ((size_t)depth == levels.size()) levels.push_back(new CpuLevel(*this, depth == 0 ? 128u << 20 : 32u << 20));
    CpuLevel* lvl = levels[depth];
    auto& regs = lvl->jit->Regs();

    u32 sp = r[13];  // the interrupted level's stack pointer, or the top of the stack when idle
    size_t nstack = nwords > 4 ? nwords - 4 : 0;
    sp -= (u32)(nstack * 4);
    sp &= ~7u;
    if (sp < stack_base + 4096) fatal("guest stack overflow");
    for (size_t i = 0; i < nstack; i++) g2h<u32>(sp)[i] = words[4 + i];

    for (size_t i = 0; i < 4; i++) regs[i] = i < nwords ? words[i] : 0;
    regs[13] = sp;
    regs[14] = g_return_trap;
    regs[15] = fn & ~1u;
    lvl->jit->SetCpsr(0x10u | ((fn & 1) << 5));  // user mode, ARM or Thumb
    if (depth > 0) lvl->jit->SetFpscr(levels[depth - 1]->jit->Fpscr());

    u32* outer = r;
    r = regs.data();
    depth++;
    lvl->returned = false;
    for (;;) {
        if (epoch != mem_code_epoch()) {
            epoch = mem_code_epoch();
            for (CpuLevel* l : levels)
                if (!l->jit->IsExecuting()) l->jit->ClearCache();
                else l->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
        lvl->jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        lvl->jit->ClearHalt(Dynarmic::HaltReason::UserDefined2);
        lvl->jit->Run();
        if (exit_pending || lvl->returned) break;
        // Otherwise: halted so that stale translations can be cleared at the top of the loop.
    }
    u64 ret = (u64)regs[0] | ((u64)regs[1] << 32);
    depth--;
    r = outer;
    if (exit_pending) {
        if (depth == 0) exit_pending = false;
        throw ThreadExit{exit_value};
    }
    return ret;
}

}  // namespace a64b
