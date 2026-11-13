//! Signals between guest threads.
//!
//! Guest code installs handlers (sigaction, signal) and sends signals to its own threads (pthread_kill,
//! raise, kill on its own pid). The Mono runtime of Unity games depends on this: its garbage collector
//! stops every other thread with a signal, each thread acknowledges from its handler and then waits in
//! sigsuspend until the collector resumes it with another signal.
//!
//! A signal for another thread is recorded in that thread's Cpu (pending_signals) and the host thread
//! is poked with a host signal reserved for this (host_signal()). The target runs the guest handler:
//!   - while it executes guest code: its JIT is halted and the handler runs right after (cpu.rs),
//!   - while it is in a short host call: when the call returns (cpu.rs, on_svc),
//!   - while it waits in a blocking host call (BlockingCall): from the host signal handler, at once,
//!     as the kernel would interrupt the wait,
//!   - while it waits in sigsuspend: sigsuspend wakes up and runs it.
//! Faults in guest code (SIGSEGV...) are not turned into guest signals.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::sync::atomic::Ordering;
use std::sync::{Mutex, Once};

use crate::cpu::{cpu_interrupt, BlockingCall, Cpu};
use crate::mem::*;
use crate::thunk::reg;
use crate::util::{errno, lock, set_errno};

const MAX_SIGNAL: i32 = 64;
const SIG_INFO: u32 = 4; // SA_SIGINFO
const RESET_HAND: u32 = 0x8000_0000; // SA_RESETHAND
const SIG_DEFAULT: u32 = 0;
const SIG_IGNORE: u32 = 1;
const FUTEX_WAIT_PRIVATE: i32 = 128;
const FUTEX_WAKE_PRIVATE: i32 = 129;

extern "C" {
    fn __libc_current_sigrtmax() -> i32;
}
// Pokes a host thread; not used by bionic or ART.
fn host_signal() -> i32 {
    unsafe { __libc_current_sigrtmax() - 5 }
}

// Guest struct sigaction (32-bit bionic): handler, mask, flags, restorer.
#[repr(C)]
#[derive(Clone, Copy)]
struct GSigaction {
    handler: u32,
    mask: u32,
    flags: u32,
    restorer: u32,
}

static ACTIONS: Mutex<[GSigaction; MAX_SIGNAL as usize + 1]> = Mutex::new([GSigaction { handler: 0, mask: 0, flags: 0, restorer: 0 }; MAX_SIGNAL as usize + 1]);

fn action(sig: i32) -> GSigaction {
    lock(&ACTIONS)[sig as usize]
}

fn wake_sigsuspend(c: &Cpu) {
    c.signal_seq.fetch_add(1, Ordering::SeqCst);
    unsafe { libc::syscall(libc::SYS_futex, c.signal_seq.as_ptr(), FUTEX_WAKE_PRIVATE, 1, 0usize, 0usize, 0) };
}

// Runs on the target host thread when another thread sent it a guest signal.
unsafe extern "C" fn host_signal_handler(_sig: i32, _info: *mut libc::siginfo_t, _ctx: *mut libc::c_void) {
    let Some(c) = Cpu::current_or_null() else { return };
    if c.pending_signals.load(Ordering::SeqCst) == 0 {
        return;
    }
    let saved_errno = errno();
    if c.in_sigsuspend.load(Ordering::SeqCst) {
        // sigsuspend wakes up and delivers it.
    } else if c.blocking.load(Ordering::SeqCst) > 0 && c.depth > 0 {
        signals_deliver_pending(c); // the thread waits in a host call: run the handler now
    } else {
        cpu_interrupt(c); // executing guest code: stop at the next block and deliver there
    }
    set_errno(saved_errno);
}

fn install_host_handler() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = host_signal_handler as *const () as usize;
        sa.sa_flags = libc::SA_SIGINFO | libc::SA_RESTART | libc::SA_ONSTACK;
        libc::sigemptyset(&mut sa.sa_mask);
        libc::sigaction(host_signal(), &sa, std::ptr::null_mut());
    });
}

// The guest's ucontext_t for a handler: uc_flags, uc_link, uc_stack (12 bytes), then uc_mcontext
// (struct sigcontext: trap_no, error_code, oldmask, r0..r10, fp, ip, sp, lr, pc, cpsr, fault_address).
const FRAME_SIZE: u32 = 1024;
const INFO_SIZE: u32 = 128;
const MCONTEXT: u32 = 20;

unsafe fn fill_frame(c: &Cpu, frame: u32, sig: i32) {
    std::ptr::write_bytes(g2h::<u8>(frame), 0, FRAME_SIZE as usize);
    wr::<i32>(frame, sig); // si_signo
    wr::<i32>(frame + 8, -6); // si_code = SI_TKILL
    let regs = frame + INFO_SIZE + MCONTEXT + 12;
    for i in 0..16 {
        wr::<u32>(regs + i * 4, c.r(i as usize));
    }
    wr::<u32>(regs + 64, c.cpsr);
}

/// Runs the guest handlers of the signals pending on this thread.
pub fn signals_deliver_pending(c: &mut Cpu) {
    loop {
        let pending = c.pending_signals.load(Ordering::SeqCst);
        if pending == 0 {
            break;
        }
        let sig = pending.trailing_zeros() as i32 + 1;
        c.pending_signals.fetch_and(!(1u64 << (sig - 1)), Ordering::SeqCst);
        let a = action(sig);
        if a.handler == SIG_DEFAULT || a.handler == SIG_IGNORE {
            continue; // default actions are not emulated
        }
        if a.flags & RESET_HAND != 0 {
            lock(&ACTIONS)[sig as usize].handler = SIG_DEFAULT;
        }
        let frame = g_calloc(1, FRAME_SIZE);
        unsafe { fill_frame(c, frame, sig) };
        if a.flags & SIG_INFO != 0 {
            c.call(a.handler, &[sig as u32, frame, frame + INFO_SIZE]);
        } else {
            c.call(a.handler, &[sig as u32]);
        }
        g_free(frame);
    }
}

/// Sends a guest signal to a guest thread. Returns 0 or an errno value.
pub fn guest_signal_send(target: *mut Cpu, host_thread: libc::pthread_t, sig: i32) -> i32 {
    if sig == 0 {
        return 0;
    }
    if !(0..=MAX_SIGNAL).contains(&sig) {
        return libc::EINVAL;
    }
    if target.is_null() {
        return libc::ESRCH;
    }
    install_host_handler();
    let t = unsafe { &mut *target };
    t.pending_signals.fetch_or(1u64 << (sig - 1), Ordering::SeqCst);
    wake_sigsuspend(t);
    let me = Cpu::current_or_null().map(|c| c as *mut Cpu).unwrap_or(std::ptr::null_mut());
    if target == me {
        signals_deliver_pending(t); // to itself: synchronously, like the kernel on return
        return 0;
    }
    unsafe { libc::pthread_kill(host_thread, host_signal()) }
}

pub fn register_signal_thunks() {
    reg("sigaction", |sig: i32, act: *const GSigaction, old: *mut GSigaction| -> i32 {
        if sig <= 0 || sig > MAX_SIGNAL {
            set_errno(libc::EINVAL);
            return -1;
        }
        let mut actions = lock(&ACTIONS);
        unsafe {
            if !old.is_null() {
                old.write_unaligned(actions[sig as usize]);
            }
            if !act.is_null() {
                actions[sig as usize] = act.read_unaligned();
            }
        }
        0
    });
    for n in ["signal", "bsd_signal", "sysv_signal"] {
        reg(n, |sig: i32, handler: u32| -> u32 {
            if sig <= 0 || sig > MAX_SIGNAL {
                return 0xFFFF_FFFF; // SIG_ERR
            }
            let mut actions = lock(&ACTIONS);
            let old = actions[sig as usize].handler;
            actions[sig as usize] = GSigaction { handler, mask: 0, flags: 0x1000_0000 /* SA_RESTART */, restorer: 0 };
            old
        });
    }

    // Waits until a signal arrives, runs its handler and returns -1 (EINTR), as sigsuspend does.
    reg("sigsuspend", |_mask: u32| -> i32 {
        let c = Cpu::current();
        c.in_sigsuspend.store(true, Ordering::SeqCst);
        while c.pending_signals.load(Ordering::SeqCst) == 0 {
            let seq = c.signal_seq.load(Ordering::SeqCst);
            if c.pending_signals.load(Ordering::SeqCst) != 0 {
                break;
            }
            let mut timeout: libc::timespec = unsafe { std::mem::zeroed() };
            timeout.tv_nsec = 100 * 1000 * 1000;
            let _blocking = BlockingCall::new();
            unsafe { libc::syscall(libc::SYS_futex, c.signal_seq.as_ptr(), FUTEX_WAIT_PRIVATE, seq, &timeout as *const libc::timespec, 0usize, 0) };
        }
        c.in_sigsuspend.store(false, Ordering::SeqCst);
        signals_deliver_pending(c);
        set_errno(libc::EINTR);
        -1
    });

    reg("raise", |sig: i32| -> i32 {
        let a = action(if sig > 0 && sig <= MAX_SIGNAL { sig } else { 0 });
        if a.handler == SIG_IGNORE {
            return 0;
        }
        if a.handler == SIG_DEFAULT {
            fatal!("guest raised signal {}", sig);
        }
        if guest_signal_send(Cpu::current(), unsafe { libc::pthread_self() }, sig) == 0 { 0 } else { -1 }
    });
    reg("kill", |pid: i32, sig: i32| -> i32 {
        unsafe {
            if pid != libc::getpid() || sig == 0 {
                return libc::kill(pid, sig);
            }
        }
        let a = action(if sig > 0 && sig <= MAX_SIGNAL { sig } else { 0 });
        if a.handler == SIG_DEFAULT {
            fatal!("guest sent signal {} to itself", sig);
        }
        if a.handler == SIG_IGNORE {
            return 0;
        }
        if guest_signal_send(Cpu::current(), unsafe { libc::pthread_self() }, sig) == 0 { 0 } else { -1 }
    });
}
