// Crash reporting: when the process receives a fatal signal, write what is known
// about the guest state of the crashing thread to the log before Android's
// debuggerd takes over. On Android, libsigchain lets ART handle its own signals
// (implicit null checks, stack overflow checks) before this handler runs.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <signal.h>
#include <unistd.h>

#include "cpu.h"

namespace a64b {

namespace {
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction g_old[32];
volatile sig_atomic_t g_in_handler = 0;

void handler(int sig, siginfo_t* info, void* uctx) {
    if (!g_in_handler) {
        g_in_handler = 1;
        LOGE("=== fatal signal %d (code %d), fault address %p ===", sig, info ? info->si_code : 0, info ? info->si_addr : nullptr);
        if (info && mem_contains((u64)(uintptr_t)info->si_addr))
            LOGE(" the fault address is inside guest memory: %s", describe_address((u32)(uintptr_t)info->si_addr).c_str());
        if (Cpu* c = Cpu::current_or_null()) cpu_dump_state(*c, "state of the crashing thread");
        else LOGE(" the crashing thread never ran guest code");
        if (int fd = log_file_fd(); fd >= 0) fsync(fd);
    }
    // Hand the signal to whoever had it before (normally debuggerd's handler).
    sigaction(sig, &g_old[sig], nullptr);
    if (sig == SIGABRT) {
        raise(sig);
        return;
    }
    (void)uctx;
    // Returning re-executes the faulting instruction, which now reaches the previous handler.
}
}  // namespace

void crash_handler_install() {
    static bool done = false;
    if (done) return;
    done = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : kSignals) sigaction(s, &sa, &g_old[s]);
}

}  // namespace a64b
