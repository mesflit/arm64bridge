// SPDX-License-Identifier: GPL-2.0-or-later
//
// Signals between guest threads.
//
// Guest code installs handlers (sigaction, signal) and sends signals to its own threads (pthread_kill,
// raise, kill on its own pid). The Mono runtime of Unity games depends on this: its garbage collector
// stops every other thread with a signal, each thread acknowledges from its handler and then waits in
// sigsuspend until the collector resumes it with another signal.
//
// A signal for another thread is recorded in that thread's Cpu (pending_signals) and the host thread
// is poked with a host signal reserved for this (kHostSignal). The target runs the guest handler:
//   - while it executes guest code: its JIT is halted and the handler runs right after (cpu.cpp),
//   - while it is in a short host call: when the call returns (cpu.cpp, on_svc),
//   - while it waits in a blocking host call (BlockingCall): from the host signal handler, at once,
//     as the kernel would interrupt the wait,
//   - while it waits in sigsuspend: sigsuspend wakes up and runs it.
// Faults in guest code (SIGSEGV...) are not turned into guest signals.

#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

#include "cpu.h"
#include "guest_mem.h"
#include "thunk.h"

namespace a64b {

namespace {

constexpr int kMaxSignal = 64;
constexpr u32 kSigInfo = 4;               // SA_SIGINFO
constexpr u32 kResetHand = 0x80000000u;   // SA_RESETHAND
constexpr u32 kSigDefault = 0, kSigIgnore = 1;
const int kHostSignal = SIGRTMAX - 5;      // pokes a host thread; not used by bionic or ART

// Guest struct sigaction (32-bit bionic): handler, mask, flags, restorer.
struct GSigaction {
    u32 handler;
    u32 mask;
    u32 flags;
    u32 restorer;
};

std::mutex g_mu;
GSigaction g_actions[kMaxSignal + 1];

GSigaction action(int sig) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_actions[sig];
}

void wake_sigsuspend(Cpu& c) {
    c.signal_seq.fetch_add(1);
    syscall(SYS_futex, reinterpret_cast<int*>(&c.signal_seq), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

// Runs on the target host thread when another thread sent it a guest signal.
void host_signal_handler(int, siginfo_t*, void*) {
    Cpu* c = Cpu::current_or_null();
    if (!c || !c->pending_signals.load()) return;
    int saved_errno = errno;
    if (c->in_sigsuspend.load()) {
        // sigsuspend wakes up and delivers it.
    } else if (c->blocking.load() > 0 && c->depth > 0) {
        signals_deliver_pending(*c);  // the thread waits in a host call: run the handler now
    } else {
        cpu_interrupt(*c);  // executing guest code: stop at the next block and deliver there
    }
    errno = saved_errno;
}

void install_host_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa{};
        sa.sa_sigaction = host_signal_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(kHostSignal, &sa, nullptr);
    });
}

// The guest's ucontext_t for a handler: uc_flags, uc_link, uc_stack (12 bytes), then uc_mcontext
// (struct sigcontext: trap_no, error_code, oldmask, r0..r10, fp, ip, sp, lr, pc, cpsr, fault_address).
constexpr u32 kFrameSize = 1024, kInfoSize = 128, kMcontext = 20;

void fill_frame(Cpu& c, u32 frame, int sig) {
    memset(g2h(frame), 0, kFrameSize);
    i32* info = g2h<i32>(frame);
    info[0] = sig;  // si_signo
    info[2] = -6;   // si_code = SI_TKILL
    u32* regs = g2h<u32>(frame + kInfoSize + kMcontext + 12);
    for (int i = 0; i < 16; i++) regs[i] = c.r[i];
    regs[16] = c.cpsr;
}

}  // namespace

void signals_deliver_pending(Cpu& c) {
    u64 pending;
    while ((pending = c.pending_signals.load()) != 0) {
        const int sig = __builtin_ctzll(pending) + 1;
        c.pending_signals.fetch_and(~(1ull << (sig - 1)));
        GSigaction a = action(sig);
        if (a.handler == kSigDefault || a.handler == kSigIgnore) continue;  // default actions are not emulated
        if (a.flags & kResetHand) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_actions[sig].handler = kSigDefault;
        }
        u32 frame = g_calloc(1, kFrameSize);
        fill_frame(c, frame, sig);
        if (a.flags & kSigInfo) c.call(a.handler, {(u32)sig, frame, frame + kInfoSize});
        else c.call(a.handler, {(u32)sig});
        g_free(frame);
    }
}

int guest_signal_send(Cpu* target, pthread_t host_thread, int sig) {
    if (sig == 0) return 0;
    if (sig < 0 || sig > kMaxSignal) return EINVAL;
    if (!target) return ESRCH;
    install_host_handler();
    target->pending_signals.fetch_or(1ull << (sig - 1));
    wake_sigsuspend(*target);
    if (target == Cpu::current_or_null()) {
        signals_deliver_pending(*target);  // to itself: synchronously, like the kernel on return
        return 0;
    }
    return pthread_kill(host_thread, kHostSignal);
}

void register_signal_thunks() {
    reg("sigaction", +[](int sig, const GSigaction* act, GSigaction* old) -> int {
        if (sig <= 0 || sig > kMaxSignal) {
            errno = EINVAL;
            return -1;
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (old) *old = g_actions[sig];
        if (act) g_actions[sig] = *act;
        return 0;
    });
    auto set_handler = +[](int sig, u32 handler) -> u32 {
        if (sig <= 0 || sig > kMaxSignal) return 0xFFFFFFFF;  // SIG_ERR
        std::lock_guard<std::mutex> lk(g_mu);
        u32 old = g_actions[sig].handler;
        g_actions[sig] = GSigaction{handler, 0, 0x10000000u /* SA_RESTART */, 0};
        return old;
    };
    reg("signal", set_handler);
    reg("bsd_signal", set_handler);
    reg("sysv_signal", set_handler);

    // Waits until a signal arrives, runs its handler and returns -1 (EINTR), as sigsuspend does.
    reg("sigsuspend", +[](const u32*) -> int {
        Cpu& c = Cpu::current();
        c.in_sigsuspend.store(true);
        while (!c.pending_signals.load()) {
            int seq = c.signal_seq.load();
            if (c.pending_signals.load()) break;
            timespec timeout{0, 100 * 1000 * 1000};
            BlockingCall blocking(&c);
            syscall(SYS_futex, reinterpret_cast<int*>(&c.signal_seq), FUTEX_WAIT_PRIVATE, seq, &timeout, nullptr, 0);
        }
        c.in_sigsuspend.store(false);
        signals_deliver_pending(c);
        errno = EINTR;
        return -1;
    });

    reg("raise", +[](int sig) -> int {
        GSigaction a = action(sig > 0 && sig <= kMaxSignal ? sig : 0);
        if (a.handler == kSigDefault || a.handler == kSigIgnore) {
            if (a.handler == kSigIgnore) return 0;
            fatal("guest raised signal %d", sig);
        }
        return guest_signal_send(&Cpu::current(), pthread_self(), sig) == 0 ? 0 : -1;
    });
    reg("kill", +[](int pid, int sig) -> int {
        if (pid != getpid() || sig == 0) return kill(pid, sig);
        GSigaction a = action(sig > 0 && sig <= kMaxSignal ? sig : 0);
        if (a.handler == kSigDefault) fatal("guest sent signal %d to itself", sig);
        if (a.handler == kSigIgnore) return 0;
        return guest_signal_send(&Cpu::current(), pthread_self(), sig) == 0 ? 0 : -1;
    });
}

}  // namespace a64b
