//! Emulated ARMv7 CPU (one per host thread) and the trap mechanism that lets guest code call
//! host functions. dynarmic translates the guest's ARMv7 code to host machine code; it is
//! reached through the small C interface in glue/dynarmic_glue.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::cell::Cell;
use std::collections::{HashMap, VecDeque};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU32, AtomicU64, AtomicU8, Ordering};
use std::sync::{Mutex, Once, OnceLock};

use crate::mem::*;
use crate::util::{lock, set_errno};

extern "C" {
    fn a64b_jit_has_fastmem() -> i32;
    fn a64b_page_table_entries() -> usize;
    fn a64b_monitor_new(processors: usize) -> *mut c_void;
    fn a64b_jit_new(cpu: *mut c_void, pid: usize, monitor: *mut c_void, page_table: *mut *mut u8, tls: *mut u32, code_cache: usize) -> *mut c_void;
    fn a64b_jit_delete(l: *mut c_void);
    fn a64b_jit_regs(l: *mut c_void) -> *mut u32;
    fn a64b_jit_ext_regs(l: *mut c_void) -> *mut u32;
    fn a64b_jit_cpsr(l: *mut c_void) -> u32;
    fn a64b_jit_set_cpsr(l: *mut c_void, v: u32);
    fn a64b_jit_fpscr(l: *mut c_void) -> u32;
    fn a64b_jit_set_fpscr(l: *mut c_void, v: u32);
    fn a64b_jit_run(l: *mut c_void);
    fn a64b_jit_halt(l: *mut c_void, reason: i32);
    fn a64b_jit_clear_halts(l: *mut c_void);
    fn a64b_jit_clear_cache(l: *mut c_void);
    fn a64b_jit_invalidate_range(l: *mut c_void, start: u32, len: u32);
    fn a64b_jit_is_executing(l: *mut c_void) -> i32;
}

/// Name of the CPU backend, reported by the self test.
pub const BACKEND_NAME: &str = "dynarmic ARMv7 recompiler";

const HALT_LEAVE: i32 = 1;
const HALT_STALE: i32 = 2;
const HALT_SIGNAL: i32 = 3;

pub static G_FRAMES: AtomicU64 = AtomicU64::new(0);
pub static G_GUEST_THREADS: AtomicI32 = AtomicI32::new(0);
/// Thread that last called eglSwapBuffers.
pub static G_RENDER_TID: AtomicI32 = AtomicI32::new(0);
/// FPS booster: 0 = leave the game's frame limiter alone, 1 = 60 FPS, 2 = no limit. Known clock-based limiters
/// get a new frame time (frame_cap.rs); for other games the render thread's sleeps are halved (1) or skipped (2).
pub static G_FRAME_LIMITER: AtomicI32 = AtomicI32::new(0);

static G_CPUS: Mutex<Vec<usize>> = Mutex::new(Vec::new());

#[inline]
fn ticks() -> u64 {
    #[cfg(target_arch = "aarch64")]
    unsafe {
        let v: u64;
        std::arch::asm!("mrs {}, cntvct_el0", out(reg) v, options(nomem, nostack));
        v
    }
    #[cfg(target_arch = "x86_64")]
    unsafe {
        std::arch::x86_64::_rdtsc()
    }
}

fn mono_ns() -> i64 {
    let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as i64 * 1_000_000_000 + ts.tv_nsec as i64
}

fn ticks_per_second() -> f64 {
    static F: OnceLock<f64> = OnceLock::new();
    *F.get_or_init(|| {
        let a = mono_ns();
        let t0 = ticks();
        let mut b = mono_ns();
        while b - a < 20_000_000 {
            b = mono_ns();
        }
        let t1 = ticks();
        (t1 - t0) as f64 / ((b - a) as f64 / 1e9)
    })
}

// ---------------------------------------------------------------------------
// Trap stubs. Slot i is "svc #(i+1); bx lr" (ARM mode): the SVC number selects the
// host function directly. "svc #0" is left for raw system calls made by guest code.
// ---------------------------------------------------------------------------
pub const MAX_TRAPS: u32 = 32768;
const STUB_SIZE: u32 = 8;

/// A host function callable by guest code.
pub type TrapFn = dyn Fn(&mut Cpu) + Send + Sync;

struct TrapEntry {
    f: &'static TrapFn,
    name: &'static str,
}

static G_TRAP_BASE: AtomicU32 = AtomicU32::new(0);
static G_TRAPS: OnceLock<Box<[OnceLock<TrapEntry>]>> = OnceLock::new();
static G_TRAP_COUNT: AtomicU32 = AtomicU32::new(0);
static G_RETURN_TRAP: AtomicU32 = AtomicU32::new(0);
static G_RETURN_IDX: AtomicU32 = AtomicU32::new(0);
static G_TRAP_CALLED: OnceLock<Box<[AtomicU8]>> = OnceLock::new();

// The ARM Linux "kuser helpers": routines the kernel provides at fixed addresses at the top of the
// address space. Old libraries (GCC's atomics, some libstdc++/bionic builds) call them directly.
const KUSER_PAGE: u32 = 0xFFFF_0000;
fn kuser_word(a: u32) -> u32 {
    const CMPXCHG64: [u32; 11] =
        [0xe92d40f0, 0xe8900030, 0xe89100c0, 0xe1b20f9f, 0xe0303004, 0x00313005, 0x01a23f96, 0x03330001, 0x0afffff9, 0xe2730000, 0xe8bd80f0];
    const BARRIER: [u32; 2] = [0xf57ff05f, 0xe12fff1e];
    const CMPXCHG: [u32; 7] = [0xe1923f9f, 0xe0533000, 0x01823f91, 0x03330001, 0x0afffffa, 0xe2730000, 0xe12fff1e];
    const GET_TLS: [u32; 2] = [0xee1d0f70, 0xe12fff1e];
    let off = (a & 0xFFC) as usize;
    if (0xF60..0xF60 + 4 * CMPXCHG64.len()).contains(&off) {
        return CMPXCHG64[(off - 0xF60) / 4]; // __kuser_cmpxchg64
    }
    if (0xFA0..0xFA0 + 4 * BARRIER.len()).contains(&off) {
        return BARRIER[(off - 0xFA0) / 4]; // __kuser_memory_barrier
    }
    if (0xFC0..0xFC0 + 4 * CMPXCHG.len()).contains(&off) {
        return CMPXCHG[(off - 0xFC0) / 4]; // __kuser_cmpxchg
    }
    if (0xFE0..0xFE0 + 4 * GET_TLS.len()).contains(&off) {
        return GET_TLS[(off - 0xFE0) / 4]; // __kuser_get_tls
    }
    if off == 0xFFC {
        return 5; // __kuser_helper_version
    }
    0xe7f000f0 // undefined instruction
}

// Identity page table: entry = host address of the guest page (guest address == host address).
static G_PAGE_TABLE: AtomicPtr<*mut u8> = AtomicPtr::new(std::ptr::null_mut());

// LDREX/STREX across guest threads.
const MAX_PROCESSORS: usize = 1024;
static G_MONITOR: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
static G_FREE_PIDS: Mutex<Vec<usize>> = Mutex::new(Vec::new());

fn take_pid() -> usize {
    match lock(&G_FREE_PIDS).pop() {
        Some(p) => p,
        None => fatal!("too many guest threads (limit {} JIT instances)", MAX_PROCESSORS),
    }
}
fn give_pid(p: usize) {
    lock(&G_FREE_PIDS).push(p);
}

/// Allocates a guest-callable stub. Calling it from guest code invokes `f` on the host.
pub fn trap_create(name: &str, f: &'static TrapFn) -> u32 {
    let idx = G_TRAP_COUNT.fetch_add(1, Ordering::SeqCst);
    if idx >= MAX_TRAPS {
        fatal!("out of trap slots");
    }
    let name: &'static str = Box::leak(name.to_string().into_boxed_str());
    let _ = G_TRAPS.get().expect("traps")[idx as usize].set(TrapEntry { f, name });
    G_TRAP_BASE.load(Ordering::Relaxed) + idx * STUB_SIZE
}

/// Stub for a plain function (convenience for callers that have no closure state).
pub fn trap_create_fn(name: &str, f: fn(&mut Cpu)) -> u32 {
    trap_create(name, Box::leak(Box::new(f)))
}

pub fn trap_name_by_index(idx: u32) -> Option<&'static str> {
    if idx >= G_TRAP_COUNT.load(Ordering::SeqCst) {
        return None;
    }
    G_TRAPS.get()?.get(idx as usize)?.get().map(|t| t.name)
}

pub fn trap_name_at(addr: u32) -> Option<&'static str> {
    let base = G_TRAP_BASE.load(Ordering::Relaxed);
    if base == 0 || addr < base || addr >= base + MAX_TRAPS * STUB_SIZE {
        return None;
    }
    trap_name_by_index((addr - base) / STUB_SIZE)
}

pub fn cpu_global_init() -> bool {
    static ONCE: Once = Once::new();
    static OK: AtomicBool = AtomicBool::new(false);
    ONCE.call_once(|| unsafe {
        if !mem_init() {
            return;
        }
        let _ = G_TRAPS.set((0..MAX_TRAPS).map(|_| OnceLock::new()).collect());
        let _ = G_TRAP_CALLED.set((0..MAX_TRAPS).map(|_| AtomicU8::new(0)).collect());
        let base = page_alloc(MAX_TRAPS * STUB_SIZE, true);
        if base == 0 {
            return;
        }
        let p = g2h::<u32>(base);
        for i in 0..MAX_TRAPS {
            *p.add(i as usize * 2) = 0xEF00_0000 | (i + 1); // svc #(i+1)
            *p.add(i as usize * 2 + 1) = 0xE12F_FF1E; // bx lr
        }
        let fastmem = a64b_jit_has_fastmem() != 0;
        if !fastmem {
            let n = a64b_page_table_entries();
            let table: &'static mut [*mut u8] = Box::leak(vec![std::ptr::null_mut::<u8>(); n].into_boxed_slice());
            for (start, size) in [(mem_arena_base(), mem_arena_size()), (mem_low_arena_base(), mem_low_arena_size())] {
                let mut a = start as u64;
                while a < start as u64 + size as u64 {
                    table[(a >> 12) as usize] = a as usize as *mut u8;
                    a += 4096;
                }
            }
            G_PAGE_TABLE.store(table.as_mut_ptr(), Ordering::SeqCst);
        }
        logi!("recompiler: dynarmic, guest memory {}", if fastmem { "direct (fastmem)" } else { "through page table" });
        G_MONITOR.store(a64b_monitor_new(MAX_PROCESSORS), Ordering::SeqCst);
        lock(&G_FREE_PIDS).extend((0..MAX_PROCESSORS).rev());
        G_TRAP_BASE.store(base, Ordering::SeqCst);
        let rt = trap_create_fn("<return>", |_| {});
        G_RETURN_TRAP.store(rt, Ordering::SeqCst);
        G_RETURN_IDX.store((rt - base) / STUB_SIZE, Ordering::SeqCst);
        OK.store(true, Ordering::SeqCst);
    });
    OK.load(Ordering::SeqCst)
}

// ---------------------------------------------------------------------------
// One JIT instance. A thread has one per nesting level: a host function called by
// guest code may call back into guest code (qsort comparators, Java calling a
// native method during a JNI call), and a dynarmic JIT cannot be re-entered.
// ---------------------------------------------------------------------------
struct CpuLevel {
    jit: *mut c_void,
    pid: usize,
    returned: bool,
}

/// Unwinds a guest thread out of its host frames when it calls pthread_exit (carried by a panic).
pub struct ThreadExit {
    pub retval: u32,
}

const DEFAULT_STACK: u32 = 2 * 1024 * 1024;

pub struct Cpu {
    /// Guest registers of the running JIT (r[13]=sp r[14]=lr r[15]=pc). Host functions read their
    /// arguments from here and write results back; there is no copying.
    regs: *mut u32,
    idle_regs: [u32; 16],
    pub cpsr: u32,
    /// handler set r[15] explicitly (with thumb bit)
    pub pc_override: bool,

    pub stack_base: u32,
    pub stack_size: u32,
    /// guest thread pointer (TPIDRURO), read directly by translated code
    pub tls_block: u32,
    /// guest errno storage
    pub errno_addr: u32,
    /// guest JNIEnv* of this thread
    pub guest_env: u32,
    /// host JNIEnv* (opaque here)
    pub host_env: *mut c_void,
    /// guest pthread_t
    pub thread_id: u32,
    pub depth: i32,
    epoch: u64,
    code_ranges_seen: u64,
    /// address of the last invalid guest memory access
    pub fault_addr: u64,
    /// 1 = read, 2 = write
    pub fault_type: i32,
    pub crash_reported: bool,
    /// created by the guest with pthread_create
    pub guest_thread: bool,
    /// guest asked to detach from the JVM; done when the thread ends
    pub detach_deferred: bool,
    /// pthread_exit was called; unwind out of the JIT first
    exit_pending: bool,
    exit_value: u32,
    /// Most recent host functions called by guest code on this thread (ring buffer of trap slots).
    trace: [u32; 32],
    trace_pos: u32,
    /// Profile of guest->host calls made by this thread (per trap slot), read by the heartbeat.
    pub tid: i32,
    pub host_calls: u64,
    prof_count: Vec<u32>,
    prof_ticks: Vec<u64>,
    pub tls_values: [u32; 256],

    // JNI local reference tokens (see jni.rs)
    pub locals: Vec<*mut c_void>,
    /// token number of each entry (ascending, never reused)
    pub local_serials: Vec<u32>,
    pub next_local_serial: u32,
    // Object arguments of finished native calls, kept alive as global references: games written
    // for Dalvik often keep such a jobject and use it in a later call.
    pub kept_refs: HashMap<u32, *mut c_void>, // token -> global reference
    pub kept_unique: VecDeque<u32>,           // tokens that own their global reference
    pub kept_alias: VecDeque<u32>,            // tokens that share an earlier one (same object again)
    pub kept_last: HashMap<u64, *mut c_void>, // (method, argument) -> global reference of the last call

    // Guest signals (signals.rs): pending signal bits (bit n-1 = signal n), how many blocking
    // host calls the thread is in, whether it waits in sigsuspend, and a counter to wake that wait.
    pub pending_signals: AtomicU64,
    pub blocking: AtomicI32,
    pub in_sigsuspend: AtomicBool,
    pub signal_seq: AtomicI32,

    levels: Vec<Box<CpuLevel>>,
}

struct CpuHolder(Cell<*mut Cpu>);
impl Drop for CpuHolder {
    fn drop(&mut self) {
        let p = self.0.replace(std::ptr::null_mut());
        if !p.is_null() {
            unsafe { drop(Box::from_raw(p)) };
        }
    }
}
thread_local! {
    static T_CPU: CpuHolder = const { CpuHolder(Cell::new(std::ptr::null_mut())) };
}

impl Cpu {
    /// The CPU of the calling thread (created on first use).
    pub fn current() -> &'static mut Cpu {
        T_CPU.with(|h| {
            if h.0.get().is_null() {
                h.0.set(Box::into_raw(Cpu::new()));
            }
            unsafe { &mut *h.0.get() }
        })
    }
    pub fn current_or_null() -> Option<&'static mut Cpu> {
        T_CPU.try_with(|h| h.0.get()).ok().filter(|p| !p.is_null()).map(|p| unsafe { &mut *p })
    }

    fn new() -> Box<Cpu> {
        if !cpu_global_init() {
            fatal!("bridge initialisation failed");
        }
        let stack_base = page_alloc(DEFAULT_STACK, false);
        let tls_block = g_calloc(1, 512);
        if stack_base == 0 || tls_block == 0 {
            fatal!("cannot allocate guest thread state");
        }
        let mut c = Box::new(Cpu {
            regs: std::ptr::null_mut(),
            idle_regs: [0; 16],
            cpsr: 0,
            pc_override: false,
            stack_base,
            stack_size: DEFAULT_STACK,
            tls_block,
            errno_addr: tls_block + 256,
            guest_env: 0,
            host_env: std::ptr::null_mut(),
            thread_id: 0,
            depth: 0,
            epoch: mem_code_epoch(),
            code_ranges_seen: mem_code_ranges_seq(),
            fault_addr: 0,
            fault_type: 0,
            crash_reported: false,
            guest_thread: false,
            detach_deferred: false,
            exit_pending: false,
            exit_value: 0,
            trace: [0; 32],
            trace_pos: 0,
            tid: crate::util::gettid(),
            host_calls: 0,
            prof_count: vec![0; MAX_TRAPS as usize],
            prof_ticks: vec![0; MAX_TRAPS as usize],
            tls_values: [0; 256],
            locals: Vec::new(),
            local_serials: Vec::new(),
            next_local_serial: 1,
            kept_refs: HashMap::new(),
            kept_unique: VecDeque::new(),
            kept_alias: VecDeque::new(),
            kept_last: HashMap::new(),
            pending_signals: AtomicU64::new(0),
            blocking: AtomicI32::new(0),
            in_sigsuspend: AtomicBool::new(false),
            signal_seq: AtomicI32::new(0),
            levels: Vec::new(),
        });
        c.idle_regs[13] = stack_base + DEFAULT_STACK - 64;
        c.regs = c.idle_regs.as_mut_ptr();
        lock(&G_CPUS).push(&mut *c as *mut Cpu as usize);
        c
    }

    /// Guest register i.
    #[inline(always)]
    pub fn r(&self, i: usize) -> u32 {
        debug_assert!(i < 16);
        unsafe { *self.regs.add(i) }
    }
    #[inline(always)]
    pub fn set_r(&mut self, i: usize, v: u32) {
        debug_assert!(i < 16);
        unsafe { *self.regs.add(i) = v }
    }
    #[inline(always)]
    pub fn regs_ptr(&self) -> *mut u32 {
        self.regs
    }
    #[inline(always)]
    pub fn set_ret32(&mut self, v: u32) {
        self.set_r(0, v)
    }
    #[inline(always)]
    pub fn set_ret64(&mut self, v: u64) {
        self.set_r(0, v as u32);
        self.set_r(1, (v >> 32) as u32);
    }
    /// Copies the host errno to the guest's.
    #[inline]
    pub fn sync_errno(&mut self) {
        unsafe { *g2h::<i32>(self.errno_addr) = crate::util::errno() }
    }
    /// Copies the guest errno to the host's (before a host function runs on behalf of the guest).
    #[inline]
    pub fn load_errno(&self) {
        set_errno(unsafe { *g2h::<i32>(self.errno_addr) })
    }
    /// VFP/NEON registers of the running JIT as 64 words (d0 = words 0,1).
    pub fn ext_regs(&mut self) -> *mut u32 {
        if self.depth <= 0 {
            return std::ptr::null_mut();
        }
        unsafe { a64b_jit_ext_regs(self.levels[self.depth as usize - 1].jit) }
    }
    pub fn exit_is_pending(&self) -> bool {
        self.exit_pending
    }

    /// Called by translated code for "svc #n" (n = trap slot + 1, 0 = raw system call).
    /// Runs inside translated code. Host functions read and write the guest registers in place.
    fn on_svc(&mut self, swi: u32) {
        let jit = self.levels[self.depth as usize - 1].jit;
        if swi == 0 {
            // raw Linux system call
            let v = crate::thunks::misc::guest_syscall(self, self.r(7));
            self.set_r(0, v);
        } else {
            let idx = swi - 1;
            if idx == G_RETURN_IDX.load(Ordering::Relaxed) {
                self.levels[self.depth as usize - 1].returned = true;
                unsafe { a64b_jit_halt(jit, HALT_LEAVE) };
                return;
            }
            let entry = if idx < G_TRAP_COUNT.load(Ordering::Acquire) { G_TRAPS.get().and_then(|t| t[idx as usize].get()) } else { None };
            let Some(t) = entry else { guest_crash(self, "call through an unassigned trap slot") };
            self.trace[(self.trace_pos % 32) as usize] = idx + 1;
            self.trace_pos = self.trace_pos.wrapping_add(1);
            self.host_calls += 1;
            let called = &G_TRAP_CALLED.get().unwrap()[idx as usize];
            if called.load(Ordering::Relaxed) == 0 && called.swap(1, Ordering::SeqCst) == 0 {
                logi!("first call: {}", t.name);
            }
            self.pc_override = false;
            self.prof_count[idx as usize] = self.prof_count[idx as usize].wrapping_add(1);
            let this: *mut Cpu = self;
            let timed = (self.host_calls & 15) == 0; // time one call in 16: reading the clock costs as much as a cheap call
            let t0 = if timed { ticks() } else { 0 };
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (t.f)(unsafe { &mut *this })));
            match result {
                Ok(()) => {
                    if timed {
                        self.prof_ticks[idx as usize] += (ticks() - t0) * 16;
                    }
                }
                Err(payload) => match payload.downcast::<ThreadExit>() {
                    Ok(e) => {
                        // A panic cannot unwind through translated code: leave the JIT first.
                        self.exit_pending = true;
                        self.exit_value = e.retval;
                        unsafe { a64b_jit_halt(jit, HALT_LEAVE) };
                        return;
                    }
                    Err(other) => {
                        let msg = other.downcast_ref::<String>().cloned().or_else(|| other.downcast_ref::<&str>().map(|s| s.to_string())).unwrap_or_default();
                        cpu_dump_state(self, &format!("internal error in host function {}: {}", t.name, msg), false);
                        fatal!("internal error in host function {}", t.name);
                    }
                },
            }
            if self.pc_override {
                // longjmp: r[15] holds the target with the Thumb bit
                let target = self.r(15);
                unsafe { a64b_jit_set_cpsr(jit, (a64b_jit_cpsr(jit) & !0x20) | ((target & 1) << 5)) };
                self.set_r(15, target & !1);
                self.pc_override = false;
            }
            // A signal that arrived during the call runs now, before the guest continues.
            if self.pending_signals.load(Ordering::Relaxed) != 0 {
                crate::signals::signals_deliver_pending(self);
            }
        }
        // Stale translations (library loaded, guest JIT flushed its cache): leave so the loop can clear them.
        if self.epoch != mem_code_epoch() || self.code_ranges_seen != mem_code_ranges_seq() {
            unsafe { a64b_jit_halt(jit, HALT_STALE) };
        }
    }

    /// Drops translations of guest code that changed: everything after a library was loaded or unloaded,
    /// only the flushed ranges after a JIT compiler inside the app generated code.
    fn sync_translations(&mut self) {
        let mut clear_all = self.epoch != mem_code_epoch();
        let mut ranges = Vec::new();
        let seq = mem_code_ranges_seq();
        if !clear_all && seq != self.code_ranges_seen {
            match mem_code_ranges_since(self.code_ranges_seen) {
                Some(r) => ranges = r,
                None => clear_all = true,
            }
        }
        self.epoch = mem_code_epoch();
        self.code_ranges_seen = seq;
        for l in &self.levels {
            unsafe {
                if clear_all {
                    if a64b_jit_is_executing(l.jit) == 0 {
                        a64b_jit_clear_cache(l.jit);
                    } else {
                        a64b_jit_halt(l.jit, HALT_STALE);
                    }
                } else {
                    for r in &ranges {
                        a64b_jit_invalidate_range(l.jit, r.0, r.1); // deferred if executing
                    }
                }
            }
        }
    }

    /// Call a guest function following AAPCS (softfp). words are already-marshalled 32-bit argument words.
    pub fn call(&mut self, func: u32, words: &[u32]) -> u64 {
        if func == 0 {
            fatal!("attempt to call a NULL guest function");
        }
        let depth = self.depth as usize;
        if depth == self.levels.len() {
            let pid = take_pid();
            let jit = unsafe {
                a64b_jit_new(
                    self as *mut Cpu as *mut c_void,
                    pid,
                    G_MONITOR.load(Ordering::Relaxed),
                    G_PAGE_TABLE.load(Ordering::Relaxed),
                    &mut self.tls_block,
                    if depth == 0 { 128 << 20 } else { 32 << 20 },
                )
            };
            self.levels.push(Box::new(CpuLevel { jit, pid, returned: false }));
        }
        let jit = self.levels[depth].jit;
        unsafe {
            let regs = a64b_jit_regs(jit);
            let mut sp = self.r(13); // the interrupted level's stack pointer, or the top of the stack when idle
            let nstack = words.len().saturating_sub(4);
            sp = sp.wrapping_sub(nstack as u32 * 4);
            sp &= !7;
            if sp < self.stack_base + 4096 {
                fatal!("guest stack overflow");
            }
            for i in 0..nstack {
                *g2h::<u32>(sp).add(i) = words[4 + i];
            }
            for i in 0..4 {
                *regs.add(i) = words.get(i).copied().unwrap_or(0);
            }
            *regs.add(13) = sp;
            *regs.add(14) = G_RETURN_TRAP.load(Ordering::Relaxed);
            *regs.add(15) = func & !1;
            a64b_jit_set_cpsr(jit, 0x10 | ((func & 1) << 5)); // user mode, ARM or Thumb
            if depth > 0 {
                a64b_jit_set_fpscr(jit, a64b_jit_fpscr(self.levels[depth - 1].jit));
            }

            let outer = self.regs;
            self.regs = regs;
            self.depth += 1;
            self.levels[depth].returned = false;
            loop {
                self.sync_translations();
                a64b_jit_clear_halts(jit);
                a64b_jit_run(jit);
                if self.exit_pending || self.levels[depth].returned {
                    break;
                }
                // Otherwise: halted so that stale translations can be cleared at the top of the loop, or so
                // that a signal sent to this thread runs between two instructions of the guest.
                if self.pending_signals.load(Ordering::Relaxed) != 0 {
                    crate::signals::signals_deliver_pending(self);
                }
            }
            let ret = *regs as u64 | ((*regs.add(1) as u64) << 32);
            self.depth -= 1;
            self.regs = outer;
            if self.exit_pending {
                if self.depth == 0 {
                    self.exit_pending = false;
                }
                std::panic::resume_unwind(Box::new(ThreadExit { retval: self.exit_value }));
            }
            ret
        }
    }

    /// Replaces the guest stack of an idle CPU (threads created with their own stack size).
    pub fn set_stack(&mut self, base: u32, size: u32) {
        self.stack_base = base;
        self.stack_size = size;
        self.idle_regs[13] = base + size - 64;
    }
    pub fn idle_sp(&self) -> u32 {
        self.idle_regs[13]
    }
    pub fn set_idle_sp(&mut self, sp: u32) {
        self.idle_regs[13] = sp;
    }
}

impl Drop for Cpu {
    fn drop(&mut self) {
        let me = self as *mut Cpu as usize;
        lock(&G_CPUS).retain(|&p| p != me);
        for l in self.levels.drain(..) {
            unsafe { a64b_jit_delete(l.jit) };
            give_pid(l.pid);
        }
        if self.stack_base != 0 {
            page_free(self.stack_base, self.stack_size);
        }
        // tls_block may have been replaced by the guest (set_tls); the original allocation is small, leak it.
    }
}

/// Runs `f`, catching the unwind of a guest thread that called pthread_exit inside it.
pub fn catch_thread_exit<R>(f: impl FnOnce() -> R) -> Result<R, u32> {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(v) => Ok(v),
        Err(p) => match p.downcast::<ThreadExit>() {
            Ok(e) => Err(e.retval),
            Err(other) => std::panic::resume_unwind(other),
        },
    }
}

/// Ends the calling guest thread: unwinds to where the thread (or the outermost guest call) started.
pub fn thread_exit(retval: u32) -> ! {
    std::panic::resume_unwind(Box::new(ThreadExit { retval }))
}

/// Total guest->host calls of all threads.
pub fn cpu_total_host_calls() -> u64 {
    lock(&G_CPUS).iter().map(|&p| unsafe { (*(p as *const Cpu)).host_calls }).sum()
}

/// Calls f for every CPU (guest thread state) of the process, under the registry lock.
pub fn cpu_for_each(mut f: impl FnMut(&mut Cpu)) {
    for &p in lock(&G_CPUS).iter() {
        f(unsafe { &mut *(p as *mut Cpu) });
    }
}

struct ProfLast {
    tid: i32,
    count: Vec<u32>,
    ticks: Vec<u64>,
}
static PROF_LAST: Mutex<ProfLast> = Mutex::new(ProfLast { tid: 0, count: Vec::new(), ticks: Vec::new() });

/// What the given thread spent its time on since the previous call (host functions by time).
pub fn cpu_profile_report(tid: i32, seconds: f64) -> String {
    let mut count: Vec<u32> = Vec::new();
    let mut tk: Vec<u64> = Vec::new();
    for &p in lock(&G_CPUS).iter() {
        let c = unsafe { &*(p as *const Cpu) };
        if c.tid == tid {
            count = c.prof_count.clone(); // racy snapshot of another thread's counters; good enough for statistics
            tk = c.prof_ticks.clone();
        }
    }
    if count.is_empty() {
        return String::new();
    }
    let mut last = lock(&PROF_LAST);
    if last.tid != tid || last.count.len() != count.len() {
        *last = ProfLast { tid, count, ticks: tk };
        return String::new();
    }
    let mut rows: Vec<(u32, u64, u64)> = Vec::new(); // slot, calls, ticks
    let (mut total_n, mut total_t) = (0u64, 0u64);
    for i in 0..count.len() {
        let n = count[i].wrapping_sub(last.count[i]) as u64;
        let t = tk[i].wrapping_sub(last.ticks[i]);
        if n == 0 {
            continue;
        }
        rows.push((i as u32, n, t));
        total_n += n;
        total_t += t;
    }
    last.count = count;
    last.ticks = tk;
    if rows.is_empty() || seconds <= 0.0 {
        return String::new();
    }
    let tps = ticks_per_second();
    rows.sort_by(|a, b| b.2.cmp(&a.2));
    let mut out = format!(
        "host functions take {:.0}% of the render thread ({} calls/s); by time:",
        total_t as f64 / tps / seconds * 100.0,
        (total_n as f64 / seconds) as u64
    );
    for r in rows.iter().take(12) {
        out += &format!(" {} {:.1}% x{}/s;", trap_name_by_index(r.0).unwrap_or("?"), r.2 as f64 / tps / seconds * 100.0, (r.1 as f64 / seconds) as u64);
    }
    rows.sort_by(|a, b| b.1.cmp(&a.1));
    out += " by count:";
    for r in rows.iter().take(8) {
        out += &format!(" {} x{}/s;", trap_name_by_index(r.0).unwrap_or("?"), (r.1 as f64 / seconds) as u64);
    }
    out
}

/// Writes the call trace and last known registers of the thread to the log.
pub fn cpu_dump_state(c: &mut Cpu, why: &str, at_fault: bool) {
    loge!("=== {} ===", why);
    if at_fault && c.fault_type != 0 {
        loge!(
            " {} at address 0x{:08x}{}",
            if c.fault_type == 2 { "write to unmapped memory" } else { "read of unmapped memory" },
            c.fault_addr,
            if c.fault_addr < 0x10000 { " (NULL pointer)" } else { "" }
        );
    }
    loge!(" registers {}:", if at_fault { "at the fault" } else { "at the last guest->host call" });
    loge!(" r0={:08x} r1={:08x} r2={:08x} r3={:08x}", c.r(0), c.r(1), c.r(2), c.r(3));
    loge!(" r4={:08x} r5={:08x} r6={:08x} r7={:08x}", c.r(4), c.r(5), c.r(6), c.r(7));
    loge!(" r8={:08x} r9={:08x} r10={:08x} r11={:08x}", c.r(8), c.r(9), c.r(10), c.r(11));
    loge!(" ip={:08x} sp={:08x} lr={:08x} pc={:08x}", c.r(12), c.r(13), c.r(14), c.r(15));
    loge!(" pc: {}", crate::elf::describe_address(c.r(15)));
    loge!(" lr: {}", crate::elf::describe_address(c.r(14)));
    let mut tr = String::new();
    for i in 0..32 {
        let v = c.trace[((c.trace_pos + i) % 32) as usize];
        if v == 0 {
            continue;
        }
        tr += trap_name_by_index(v - 1).unwrap_or("?");
        tr.push(' ');
    }
    loge!(" last host calls (oldest first): {}", tr);
    // Heuristic backtrace: code addresses found on the guest stack.
    let sp = c.r(13);
    let stack_end = c.stack_base as u64 + c.stack_size as u64;
    let on_stack = sp >= c.stack_base && (sp as u64) < stack_end;
    unsafe {
        if on_stack {
            let mut found = 0;
            let mut a = sp as u64;
            while a + 4 <= stack_end && a < sp as u64 + 16384 && found < 20 {
                let v: u32 = rd(a as u32);
                let d = crate::elf::describe_address(v & !1);
                if d.contains(".so+") {
                    loge!("  stack sp+0x{:04x}: {}", a - sp as u64, d);
                    found += 1;
                }
                a += 4;
            }
        }
        // Text the crashing code was working with: strings reachable from the stack and registers
        // (error messages, file names...). One level of indirection covers string objects.
        let mut seen: Vec<String> = Vec::new();
        let mut try_string = |p: u32, seen: &mut Vec<String>| {
            if !mem_is_mapped(p, 8) {
                return;
            }
            let mut n = 0u32;
            while n < 300 && mem_is_mapped(p.wrapping_add(n), 1) {
                let b = *g2h::<u8>(p + n);
                if !(0x20..0x7f).contains(&b) {
                    break;
                }
                n += 1;
            }
            if n < 6 || n >= 300 || !mem_is_mapped(p.wrapping_add(n), 1) || *g2h::<u8>(p + n) != 0 {
                return;
            }
            let s = String::from_utf8_lossy(std::slice::from_raw_parts(g2h::<u8>(p), n as usize)).into_owned();
            if !seen.contains(&s) && seen.len() < 40 {
                seen.push(s);
            }
        };
        let mut consider = |v: u32, seen: &mut Vec<String>| {
            try_string(v, seen);
            if (v & 3) == 0 && mem_is_mapped(v, 16) {
                for k in 0..4 {
                    try_string(rd::<u32>(v + k * 4), seen);
                }
            }
        };
        for i in 0..13 {
            consider(c.r(i), &mut seen);
        }
        if on_stack {
            let mut a = sp as u64;
            while a + 4 <= stack_end && a < sp as u64 + 8192 {
                consider(rd::<u32>(a as u32), &mut seen);
                a += 4;
            }
        }
        for s in &seen {
            loge!("  text: {}", s);
        }
    }
    c.crash_reported = true;
}

pub fn guest_crash(c: &mut Cpu, why: &str) -> ! {
    cpu_dump_state(c, &format!("guest crash: {}", why), true);
    fatal!("guest code crashed ({})", why);
}

/// Stops the thread's running JIT at the next block boundary (safe from a signal handler on that thread).
pub fn cpu_interrupt(c: &Cpu) {
    if c.depth > 0 {
        if let Some(l) = c.levels.get(c.depth as usize - 1) {
            unsafe { a64b_jit_halt(l.jit, HALT_SIGNAL) };
        }
    }
}

/// Marks a host call that may wait for a long time (locks, sleeps, I/O). Signals sent to the thread
/// meanwhile are delivered during the wait, as the kernel would (see signals.rs).
pub struct BlockingCall(Option<&'static Cpu>);
impl BlockingCall {
    pub fn new() -> Self {
        let c: Option<&'static Cpu> = Cpu::current_or_null().map(|c| &*c);
        if let Some(c) = c {
            c.blocking.fetch_add(1, Ordering::Relaxed);
        }
        BlockingCall(c)
    }
}
impl Default for BlockingCall {
    fn default() -> Self {
        Self::new()
    }
}
impl Drop for BlockingCall {
    fn drop(&mut self) {
        if let Some(c) = self.0 {
            c.blocking.fetch_sub(1, Ordering::Relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// Callbacks from translated code (through glue/dynarmic_glue.cpp).
// ---------------------------------------------------------------------------
unsafe fn bad_access(cpu: *mut c_void, vaddr: u32, ty: i32) -> ! {
    let c = &mut *(cpu as *mut Cpu);
    c.fault_addr = vaddr as u64;
    c.fault_type = ty;
    guest_crash(c, if ty == 2 { "invalid memory write" } else { "invalid memory read" })
}

#[no_mangle]
unsafe extern "C" fn a64b_rs_read_code(_cpu: *mut c_void, a: u32, out: *mut u32) -> bool {
    if a >= KUSER_PAGE {
        *out = kuser_word(a);
        return true;
    }
    if !mem_contains(a as u64) {
        return false;
    }
    *out = rd::<u32>(a);
    true
}

// Only reached for addresses outside the page table (i.e. outside guest memory).
#[no_mangle]
unsafe extern "C" fn a64b_rs_mem_read(cpu: *mut c_void, a: u32, size: u32) -> u64 {
    if !mem_contains(a as u64) || !mem_contains(a as u64 + size as u64 - 1) {
        bad_access(cpu, a, 1);
    }
    match size {
        1 => rd::<u8>(a) as u64,
        2 => rd::<u16>(a) as u64,
        4 => rd::<u32>(a) as u64,
        _ => rd::<u64>(a),
    }
}

#[no_mangle]
unsafe extern "C" fn a64b_rs_mem_write(cpu: *mut c_void, a: u32, v: u64, size: u32) {
    if !mem_contains(a as u64) || !mem_contains(a as u64 + size as u64 - 1) {
        bad_access(cpu, a, 2);
    }
    match size {
        1 => wr::<u8>(a, v as u8),
        2 => wr::<u16>(a, v as u16),
        4 => wr::<u32>(a, v as u32),
        _ => wr::<u64>(a, v),
    }
}

#[no_mangle]
unsafe extern "C" fn a64b_rs_mem_cas(cpu: *mut c_void, a: u32, v: u64, e: u64, size: u32) -> bool {
    use std::sync::atomic::{AtomicU16, AtomicU8};
    if !mem_contains(a as u64) {
        bad_access(cpu, a, 2);
    }
    let o = Ordering::SeqCst;
    match size {
        1 => (*g2h::<AtomicU8>(a)).compare_exchange(e as u8, v as u8, o, o).is_ok(),
        2 => (*g2h::<AtomicU16>(a)).compare_exchange(e as u16, v as u16, o, o).is_ok(),
        4 => (*g2h::<AtomicU32>(a)).compare_exchange(e as u32, v as u32, o, o).is_ok(),
        _ => (*g2h::<AtomicU64>(a)).compare_exchange(e, v, o, o).is_ok(),
    }
}

#[no_mangle]
unsafe extern "C" fn a64b_rs_svc(cpu: *mut c_void, swi: u32) {
    (*(cpu as *mut Cpu)).on_svc(swi)
}

#[no_mangle]
unsafe extern "C" fn a64b_rs_fallback(cpu: *mut c_void, pc: u32) {
    let c = &mut *(cpu as *mut Cpu);
    c.set_r(15, pc);
    guest_crash(c, "instruction not supported by the translator")
}

#[no_mangle]
unsafe extern "C" fn a64b_rs_exception(cpu: *mut c_void, pc: u32, e: i32) {
    let c = &mut *(cpu as *mut Cpu);
    c.set_r(15, pc);
    guest_crash(c, &format!("CPU exception {} (undefined or unpredictable instruction)", e))
}
