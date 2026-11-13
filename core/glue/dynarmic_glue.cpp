// C interface to dynarmic (the ARMv7 -> host recompiler) for the Rust runtime.
// Everything else of the bridge is Rust; this file only adapts dynarmic's C++ API.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

extern "C" {
// Implemented in src/cpu.rs. `cpu` is the Cpu that owns the JIT.
bool a64b_rs_read_code(void* cpu, uint32_t addr, uint32_t* out);
uint64_t a64b_rs_mem_read(void* cpu, uint32_t addr, uint32_t size);
void a64b_rs_mem_write(void* cpu, uint32_t addr, uint64_t value, uint32_t size);
bool a64b_rs_mem_cas(void* cpu, uint32_t addr, uint64_t value, uint64_t expected, uint32_t size);
void a64b_rs_svc(void* cpu, uint32_t swi);
void a64b_rs_fallback(void* cpu, uint32_t pc);
void a64b_rs_exception(void* cpu, uint32_t pc, int exception);
}

namespace {

template <class T>
bool set_fastmem_base0(T& field) {
    if constexpr (std::is_pointer_v<T>) {
        (void)field;
        return false;
    } else {
        field = 0;
        return true;
    }
}

// CP15: only the user-space thread ID registers exist.
class Cp15 final : public Dynarmic::A32::Coprocessor {
  public:
    explicit Cp15(uint32_t* tls) : tls_(tls) {}
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
    uint32_t* tls_;
    uint32_t scratch_ = 0;
};

struct Level final : Dynarmic::A32::UserCallbacks {
    void* cpu;
    std::unique_ptr<Dynarmic::A32::Jit> jit;

    explicit Level(void* c) : cpu(c) {}

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t a) override {
        uint32_t v;
        if (!a64b_rs_read_code(cpu, a, &v)) return std::nullopt;
        return v;
    }
    // Only reached for addresses outside the page table (i.e. outside guest memory).
    std::uint8_t MemoryRead8(std::uint32_t a) override { return (std::uint8_t)a64b_rs_mem_read(cpu, a, 1); }
    std::uint16_t MemoryRead16(std::uint32_t a) override { return (std::uint16_t)a64b_rs_mem_read(cpu, a, 2); }
    std::uint32_t MemoryRead32(std::uint32_t a) override { return (std::uint32_t)a64b_rs_mem_read(cpu, a, 4); }
    std::uint64_t MemoryRead64(std::uint32_t a) override { return a64b_rs_mem_read(cpu, a, 8); }
    void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { a64b_rs_mem_write(cpu, a, v, 1); }
    void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { a64b_rs_mem_write(cpu, a, v, 2); }
    void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { a64b_rs_mem_write(cpu, a, v, 4); }
    void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { a64b_rs_mem_write(cpu, a, v, 8); }
    bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override { return a64b_rs_mem_cas(cpu, a, v, e, 1); }
    bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override { return a64b_rs_mem_cas(cpu, a, v, e, 2); }
    bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override { return a64b_rs_mem_cas(cpu, a, v, e, 4); }
    bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) override { return a64b_rs_mem_cas(cpu, a, v, e, 8); }

    void InterpreterFallback(std::uint32_t pc, size_t) override { a64b_rs_fallback(cpu, pc); }
    void CallSVC(std::uint32_t swi) override { a64b_rs_svc(cpu, swi); }
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override { a64b_rs_exception(cpu, pc, (int)e); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1u << 30; }
};

using PageTable = std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

Dynarmic::HaltReason reason(int r) {
    return r == 3 ? Dynarmic::HaltReason::UserDefined3 : r == 2 ? Dynarmic::HaltReason::UserDefined2 : Dynarmic::HaltReason::UserDefined1;
}

}  // namespace

extern "C" {

// Whether translated code accesses guest memory directly (guest address == host address).
// Older dynarmic versions cannot express a fastmem base of 0; they need the identity page table.
int a64b_jit_has_fastmem(void) {
    Dynarmic::A32::UserConfig cfg{};
    return set_fastmem_base0(cfg.fastmem_pointer) ? 1 : 0;
}

size_t a64b_page_table_entries(void) { return Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES; }

void* a64b_monitor_new(size_t processors) { return new Dynarmic::ExclusiveMonitor(processors); }

// page_table: identity table of a64b_page_table_entries() host pointers (used without fastmem).
void* a64b_jit_new(void* cpu, size_t processor_id, void* monitor, uint8_t** page_table, uint32_t* tls, size_t code_cache) {
    auto* lvl = new Level(cpu);
    Dynarmic::A32::UserConfig cfg{};
    cfg.callbacks = lvl;
    cfg.processor_id = processor_id;
    cfg.global_monitor = static_cast<Dynarmic::ExclusiveMonitor*>(monitor);
    if (!set_fastmem_base0(cfg.fastmem_pointer)) cfg.page_table = reinterpret_cast<PageTable*>(page_table);
    // Games do not depend on exact NaN payloads or FMA fusing; these make VFP/NEON code cheaper.
    cfg.unsafe_optimizations = true;
    cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA | Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                         Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    cfg.coprocessors[15] = std::make_shared<Cp15>(tls);
    cfg.enable_cycle_counting = false;
    cfg.code_cache_size = code_cache;
    lvl->jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
    return lvl;
}

void a64b_jit_delete(void* l) { delete static_cast<Level*>(l); }
uint32_t* a64b_jit_regs(void* l) { return static_cast<Level*>(l)->jit->Regs().data(); }
uint32_t* a64b_jit_ext_regs(void* l) { return static_cast<Level*>(l)->jit->ExtRegs().data(); }
uint32_t a64b_jit_cpsr(void* l) { return static_cast<Level*>(l)->jit->Cpsr(); }
void a64b_jit_set_cpsr(void* l, uint32_t v) { static_cast<Level*>(l)->jit->SetCpsr(v); }
uint32_t a64b_jit_fpscr(void* l) { return static_cast<Level*>(l)->jit->Fpscr(); }
void a64b_jit_set_fpscr(void* l, uint32_t v) { static_cast<Level*>(l)->jit->SetFpscr(v); }
void a64b_jit_run(void* l) { static_cast<Level*>(l)->jit->Run(); }
// reason: 1 = leave the JIT (return / thread exit), 2 = stale translations, 3 = pending signal.
void a64b_jit_halt(void* l, int r) { static_cast<Level*>(l)->jit->HaltExecution(reason(r)); }
void a64b_jit_clear_halts(void* l) {
    auto& jit = *static_cast<Level*>(l)->jit;
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined2);
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined3);
}
void a64b_jit_clear_cache(void* l) { static_cast<Level*>(l)->jit->ClearCache(); }
void a64b_jit_invalidate_range(void* l, uint32_t start, uint32_t len) { static_cast<Level*>(l)->jit->InvalidateCacheRange(start, len); }
int a64b_jit_is_executing(void* l) { return static_cast<Level*>(l)->jit->IsExecuting() ? 1 : 0; }

}  // extern "C"
