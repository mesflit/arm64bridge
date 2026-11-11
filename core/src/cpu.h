// Emulated ARMv7 CPU (one per host thread) and the trap mechanism that lets
// guest code call host functions.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>

#include "common.h"
#include "guest_mem.h"

struct uc_struct;

namespace a64b {

struct Cpu;
using TrapFn = void (*)(Cpu& c, void* ud);

// Allocates a guest-callable stub. Calling it from guest code invokes fn on the host.
u32 trap_create(const char* name, TrapFn fn, void* ud);
const char* trap_name_at(u32 addr);
const char* trap_name_by_index(u32 idx);
// Writes the call trace and last known registers of the current thread to the log.
void cpu_dump_state(Cpu& c, const char* why, bool at_fault = false);

struct ThreadExit {
    u32 retval;
};

struct Cpu {
    uc_struct* uc = nullptr;
    // Register snapshot, valid inside trap handlers. r[13]=sp r[14]=lr r[15]=pc.
    u32 r[16] = {};
    u32 cpsr = 0;
    bool regs_dirty = false;   // handler changed more than r0-r3
    bool pc_override = false;  // handler set r[15] explicitly (with thumb bit)

    u32 stack_base = 0, stack_size = 0;
    u32 tls_block = 0;     // guest thread pointer (TPIDRURO)
    u32 errno_addr = 0;    // guest errno storage
    u32 guest_env = 0;     // guest JNIEnv* of this thread
    void* host_env = nullptr;  // host JNIEnv* (opaque here)
    u32 thread_id = 0;     // guest pthread_t
    int depth = 0;
    u64 epoch = 0;
    bool trap_pending = false;
    u32 bad_excp = 0;
    u64 fault_addr = 0;     // address of the last invalid guest memory access
    int fault_type = 0;
    bool crash_reported = false;
    // Most recent host functions called by guest code on this thread (ring buffer of trap slots).
    u32 trace[32] = {};
    u32 trace_pos = 0;
    u32 tls_values[256] = {};

    // JNI local reference tokens (see jni_bridge.cpp)
    std::vector<void*> locals;
    std::vector<u32> local_free;

    static Cpu& current();
    static Cpu* current_or_null();

    // Call a guest function following AAPCS (softfp). words are already-marshalled 32-bit argument words.
    u64 call(u32 fn, const u32* words, size_t nwords);
    u64 call(u32 fn, std::initializer_list<u32> words) { return call(fn, words.begin(), words.size()); }

    void set_ret32(u32 v) { r[0] = v; }
    void set_ret64(u64 v) { r[0] = (u32)v; r[1] = (u32)(v >> 32); }
    void sync_errno();

    ~Cpu();

  private:
    Cpu();
    void run(u32 pc);
    void load_regs();
    void store_regs(bool all);
    friend struct CpuHolder;
};

bool cpu_global_init();
[[noreturn]] void guest_crash(Cpu& c, const char* why);
// Implemented by the loader: "libfoo.so+0x1234 (symbol)".
std::string describe_address(u32 addr);

// Global interpreter lock: only one thread executes guest code at a time so that
// guest atomics (LDREX/STREX) behave correctly. Released while in host handlers.
void gil_acquire(Cpu* c);
void gil_release();

}  // namespace a64b
