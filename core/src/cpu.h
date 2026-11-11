// Emulated ARMv7 CPU (one per host thread) and the trap mechanism that lets
// guest code call host functions.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <deque>
#include <unordered_map>
#include <string>
#include <vector>

#include "common.h"
#include "guest_mem.h"

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

struct CpuLevel;  // one JIT instance; nested guest calls get their own (see cpu.cpp)

struct Cpu {
    // Guest registers of the running JIT (r[13]=sp r[14]=lr r[15]=pc). Host functions read their
    // arguments from here and write results back; there is no copying.
    u32* r = idle_regs;
    u32 idle_regs[16] = {};
    u32 cpsr = 0;              // filled in for crash reports
    bool regs_dirty = false;   // kept for handlers; unused by the dynarmic backend
    bool pc_override = false;  // handler set r[15] explicitly (with thumb bit)

    u32 stack_base = 0, stack_size = 0;
    u32 tls_block = 0;     // guest thread pointer (TPIDRURO), read directly by translated code
    u32 errno_addr = 0;    // guest errno storage
    u32 guest_env = 0;     // guest JNIEnv* of this thread
    void* host_env = nullptr;  // host JNIEnv* (opaque here)
    u32 thread_id = 0;     // guest pthread_t
    int depth = 0;
    u64 epoch = 0;
    u64 fault_addr = 0;     // address of the last invalid guest memory access
    int fault_type = 0;     // 1 = read, 2 = write
    bool crash_reported = false;
    bool guest_thread = false;    // created by the guest with pthread_create
    bool detach_deferred = false; // guest asked to detach from the JVM; done when the thread ends
    bool exit_pending = false;  // pthread_exit was called; unwind out of the JIT first
    u32 exit_value = 0;
    // Most recent host functions called by guest code on this thread (ring buffer of trap slots).
    u32 trace[32] = {};
    u32 trace_pos = 0;
    // Profile of guest->host calls made by this thread (per trap slot), read by the heartbeat.
    int tid = 0;
    u64 host_calls = 0;
    std::vector<u32> prof_count;
    std::vector<u64> prof_ticks;
    u32 tls_values[256] = {};

    // JNI local reference tokens (see jni_bridge.cpp)
    std::vector<void*> locals;
    std::vector<u32> local_serials;   // token number of each entry (ascending, never reused)
    u32 next_local_serial = 1;
    // Object arguments of finished native calls, kept alive as global references: games written
    // for Dalvik often keep such a jobject and use it in a later call.
    std::unordered_map<u32, void*> kept_refs;      // token -> global reference
    std::deque<u32> kept_unique;                    // tokens that own their global reference
    std::deque<u32> kept_alias;                     // tokens that share an earlier one (same object again)
    std::unordered_map<u64, void*> kept_last;       // (method, argument) -> global reference of the last call

    static Cpu& current();
    static Cpu* current_or_null();

    // Call a guest function following AAPCS (softfp). words are already-marshalled 32-bit argument words.
    u64 call(u32 fn, const u32* words, size_t nwords);
    u64 call(u32 fn, std::initializer_list<u32> words) { return call(fn, words.begin(), words.size()); }

    void set_ret32(u32 v) { r[0] = v; }
    void set_ret64(u64 v) { r[0] = (u32)v; r[1] = (u32)(v >> 32); }
    void sync_errno();
    // VFP/NEON registers of the running JIT as 64 words (d0 = words 0,1).
    u32* ext_regs();

    ~Cpu();

    // Called by translated code for "svc #n" (n = trap slot + 1, 0 = raw system call).
    void on_svc(u32 swi);

  private:
    Cpu();
    std::vector<CpuLevel*> levels;
    friend struct CpuHolder;
};

bool cpu_global_init();
// Total guest->host calls of all threads.
u64 cpu_total_host_calls();
// What the given thread spent its time on since the previous call (host functions by time).
std::string cpu_profile_report(int tid, double seconds);
extern std::atomic<u64> g_frames;
extern std::atomic<int> g_guest_threads;
// FPS booster: 0 = leave the game's frame limiter alone, 1 = 60 FPS, 2 = no limit. Known clock-based limiters
// get a new frame time (frame_cap.cpp); for other games the render thread's sleeps are halved (1) or skipped (2).
extern std::atomic<int> g_frame_limiter;
// Frame limiter probe (limiter_probe.cpp). kind: 0 gettimeofday, 1 clock_gettime, 2 sleep, 3 sched_yield.
void probe_time_call(int kind, u64 sleep_us = 0);
void probe_frame();
// FPS booster for clock-based frame limiters (frame_cap.cpp).
struct Module;
void frame_cap_scan(Module* m);
void frame_cap_on_clock(Cpu& c);
bool frame_cap_known();
extern std::atomic<int> g_render_tid;  // thread that last called eglSwapBuffers
[[noreturn]] void guest_crash(Cpu& c, const char* why);
// Implemented by the loader: "libfoo.so+0x1234 (symbol)".
std::string describe_address(u32 addr);

// Guest threads run in parallel, each on its own JIT. These remain as no-ops so that host
// functions can keep marking the places where they may block.
struct GilUnlock {
    explicit GilUnlock(Cpu*) {}
};

}  // namespace a64b
