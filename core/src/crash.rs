//! Crash reporting: when the process receives a fatal signal, write what is known
//! about the guest state of the crashing thread to the log before Android's
//! debuggerd takes over. On Android, libsigchain lets ART handle its own signals
//! (implicit null checks, stack overflow checks) before this handler runs.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Once;

use crate::cpu::{cpu_dump_state, Cpu};
use crate::util::Racy;

const SIGNALS: [i32; 5] = [libc::SIGSEGV, libc::SIGBUS, libc::SIGILL, libc::SIGFPE, libc::SIGABRT];
// Written once by crash_handler_install, read by the handler.
static OLD: Racy<[Option<libc::sigaction>; 32]> = Racy::new([None; 32]);
static IN_HANDLER: AtomicBool = AtomicBool::new(false);

unsafe extern "C" fn handler(sig: i32, info: *mut libc::siginfo_t, _uctx: *mut libc::c_void) {
    if !IN_HANDLER.swap(true, Ordering::SeqCst) {
        let (code, addr) = if info.is_null() { (0, 0usize) } else { ((*info).si_code, (*info).si_addr() as usize) };
        loge!("=== fatal signal {} (code {}), fault address 0x{:x} ===", sig, code, addr);
        if !info.is_null() && crate::mem::mem_contains(addr as u64) {
            loge!(" the fault address is inside guest memory: {}", crate::elf::describe_address(addr as u32));
        }
        match Cpu::current_or_null() {
            Some(c) if c.crash_reported => loge!(" (guest state was reported above)"),
            Some(c) => cpu_dump_state(c, "state of the crashing thread", false),
            None => loge!(" the crashing thread never ran guest code"),
        }
        let fd = crate::log::file_fd();
        if fd >= 0 {
            libc::fsync(fd);
        }
    }
    // Hand the signal to whoever had it before (normally debuggerd's handler).
    if let Some(old) = OLD.get()[sig as usize & 31].as_ref() {
        libc::sigaction(sig, old, std::ptr::null_mut());
    } else {
        libc::signal(sig, libc::SIG_DFL);
    }
    if sig == libc::SIGABRT {
        libc::raise(sig);
    }
    // Returning re-executes the faulting instruction, which now reaches the previous handler.
}

/// Installs handlers that write a crash report to the log before the process dies.
pub fn crash_handler_install() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = handler as *const () as usize;
        sa.sa_flags = libc::SA_SIGINFO | libc::SA_ONSTACK;
        libc::sigemptyset(&mut sa.sa_mask);
        for s in SIGNALS {
            let mut old: libc::sigaction = std::mem::zeroed();
            if libc::sigaction(s, &sa, &mut old) == 0 {
                OLD.get()[s as usize & 31] = Some(old);
            }
        }
    });
}
