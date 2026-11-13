//! pthreads, semaphores and thread-local storage for guest code.
//! Guest synchronisation objects are only 4 bytes wide (32-bit bionic); the word
//! holds an index into a table of host objects, created lazily so that static
//! initialisers (PTHREAD_MUTEX_INITIALIZER etc.) work.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::os::raw::{c_char, c_void};
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicUsize, Ordering};
use std::sync::Mutex;

use crate::cpu::{catch_thread_exit, thread_exit, BlockingCall, Cpu, G_GUEST_THREADS};
use crate::guest_types::*;
use crate::mem::*;
use crate::thunk::{reg, reg_raw, ArgReader};
use crate::util::{lock, RecMutex};

static MUTEXES: HandleTable<libc::pthread_mutex_t> = HandleTable::new();
static CONDS: HandleTable<libc::pthread_cond_t> = HandleTable::new();
static RWLOCKS: HandleTable<libc::pthread_rwlock_t> = HandleTable::new();
static SEMS: HandleTable<libc::sem_t> = HandleTable::new();

#[inline]
unsafe fn word(addr: u32) -> &'static AtomicU32 {
    &*g2h::<AtomicU32>(addr)
}

// Guest word encodings: bit0 set => (index << 1) | 1, otherwise a static initialiser.
// Returns the host object of a guest word, creating it with `make(initial word)` on first use.
unsafe fn lazy_object<T>(table: &HandleTable<T>, addr: u32, make: impl Fn(u32) -> *mut T, destroy: impl Fn(*mut T)) -> *mut T {
    let w = word(addr);
    loop {
        let mut v = w.load(Ordering::Acquire);
        if v & 1 != 0 {
            let o = table.get(v >> 1);
            if !o.is_null() {
                return o;
            }
            v = 0; // stale/destroyed: recreate with default attributes
        }
        let o = make(v);
        let idx = table.put(o);
        let expect = w.load(Ordering::SeqCst);
        if ((expect & 1) == 0 || table.get(expect >> 1).is_null()) && w.compare_exchange(expect, (idx << 1) | 1, Ordering::SeqCst, Ordering::SeqCst).is_ok() {
            return o;
        }
        table.take(idx);
        destroy(o);
    }
}

unsafe fn get_mutex(addr: u32) -> *mut libc::pthread_mutex_t {
    lazy_object(
        &MUTEXES,
        addr,
        |v| {
            let m: *mut libc::pthread_mutex_t = Box::into_raw(Box::new(std::mem::zeroed()));
            let mut at: libc::pthread_mutexattr_t = std::mem::zeroed();
            libc::pthread_mutexattr_init(&mut at);
            let ty = (v >> 14) & 3; // bionic: 0 normal, 1 recursive, 2 errorcheck
            libc::pthread_mutexattr_settype(
                &mut at,
                match ty {
                    1 => libc::PTHREAD_MUTEX_RECURSIVE,
                    2 => libc::PTHREAD_MUTEX_ERRORCHECK,
                    _ => libc::PTHREAD_MUTEX_NORMAL,
                },
            );
            libc::pthread_mutex_init(m, &at);
            libc::pthread_mutexattr_destroy(&mut at);
            m
        },
        |m| {
            libc::pthread_mutex_destroy(m);
            drop(Box::from_raw(m));
        },
    )
}

unsafe fn get_cond(addr: u32) -> *mut libc::pthread_cond_t {
    lazy_object(
        &CONDS,
        addr,
        |v| {
            let cv: *mut libc::pthread_cond_t = Box::into_raw(Box::new(std::mem::zeroed()));
            let mut at: libc::pthread_condattr_t = std::mem::zeroed();
            libc::pthread_condattr_init(&mut at);
            if v & 2 != 0 {
                libc::pthread_condattr_setclock(&mut at, libc::CLOCK_MONOTONIC);
            }
            libc::pthread_cond_init(cv, &at);
            libc::pthread_condattr_destroy(&mut at);
            cv
        },
        |cv| {
            libc::pthread_cond_destroy(cv);
            drop(Box::from_raw(cv));
        },
    )
}

unsafe fn get_rwlock(addr: u32) -> *mut libc::pthread_rwlock_t {
    lazy_object(
        &RWLOCKS,
        addr,
        |_| {
            let l: *mut libc::pthread_rwlock_t = Box::into_raw(Box::new(std::mem::zeroed()));
            libc::pthread_rwlock_init(l, std::ptr::null());
            l
        },
        |l| {
            libc::pthread_rwlock_destroy(l);
            drop(Box::from_raw(l));
        },
    )
}

unsafe fn get_sem(addr: u32, create: bool, initial: u32) -> *mut libc::sem_t {
    let w = word(addr);
    let v = w.load(Ordering::SeqCst);
    if !create && (v & 1) != 0 {
        let s = SEMS.get(v >> 1);
        if !s.is_null() {
            return s;
        }
    }
    let s: *mut libc::sem_t = Box::into_raw(Box::new(std::mem::zeroed()));
    libc::sem_init(s, 0, if create { initial } else { v >> 1 });
    w.store((SEMS.put(s) << 1) | 1, Ordering::SeqCst);
    s
}

unsafe fn destroy_object<T>(table: &HandleTable<T>, addr: u32, destroy: impl Fn(*mut T)) {
    let v: u32 = rd(addr);
    if v & 1 != 0 {
        let h = table.take(v >> 1);
        if !h.is_null() {
            destroy(h);
            drop(Box::from_raw(h));
        }
    }
    wr::<u32>(addr, 0);
}

unsafe fn abs_from_guest(g: *const GTimespec) -> libc::timespec {
    ts_from_guest(&g.read_unaligned())
}

fn now(clock: libc::clockid_t) -> libc::timespec {
    let mut t: libc::timespec = unsafe { std::mem::zeroed() };
    unsafe { libc::clock_gettime(clock, &mut t) };
    t
}

fn realtime_after(rel: GTimespec) -> libc::timespec {
    let mut t = now(libc::CLOCK_REALTIME);
    t.tv_sec += rel.tv_sec as libc::time_t;
    t.tv_nsec += rel.tv_nsec as libc::c_long;
    if t.tv_nsec >= 1_000_000_000 {
        t.tv_sec += 1;
        t.tv_nsec -= 1_000_000_000;
    }
    t
}

fn ms_rel(ms: u32) -> GTimespec {
    GTimespec { tv_sec: (ms / 1000) as i32, tv_nsec: (ms % 1000) as i32 * 1_000_000 }
}

// Converts an absolute CLOCK_MONOTONIC deadline to CLOCK_REALTIME.
fn mono_to_real(abs_mono: GTimespec) -> libc::timespec {
    let (mono, real) = (now(libc::CLOCK_MONOTONIC), now(libc::CLOCK_REALTIME));
    let mut ns = (abs_mono.tv_sec as i64 - mono.tv_sec as i64) * 1_000_000_000 + (abs_mono.tv_nsec as i64 - mono.tv_nsec as i64);
    if ns < 0 {
        ns = 0;
    }
    ns += real.tv_sec as i64 * 1_000_000_000 + real.tv_nsec as i64;
    let mut out: libc::timespec = unsafe { std::mem::zeroed() };
    out.tv_sec = (ns / 1_000_000_000) as _;
    out.tv_nsec = (ns % 1_000_000_000) as _;
    out
}

// ---- threads ----
pub struct ThreadRec {
    pub host: AtomicUsize, // pthread_t
    func: u32,
    arg: u32,
    retval: AtomicU32,
    detached: AtomicBool,
    /// while the thread runs: where signals for it go
    pub cpu: AtomicPtr<Cpu>,
}
pub static THREADS: HandleTable<ThreadRec> = HandleTable::new();

// ---- thread-specific keys ----
const MAX_KEYS: usize = 256;
#[derive(Clone, Copy)]
struct KeySlot {
    used: bool,
    dtor: u32,
}
static KEYS: Mutex<[KeySlot; MAX_KEYS]> = Mutex::new([KeySlot { used: false, dtor: 0 }; MAX_KEYS]);

fn run_key_destructors(c: &mut Cpu) {
    for _round in 0..4 {
        let mut any = false;
        for k in 0..MAX_KEYS {
            let v = c.tls_values[k];
            let d = {
                let keys = lock(&KEYS);
                if keys[k].used { keys[k].dtor } else { 0 }
            };
            if v != 0 && d != 0 {
                c.tls_values[k] = 0;
                c.call(d, &[v]);
                any = true;
            }
        }
        if !any {
            break;
        }
    }
}

extern "C" fn thread_main(p: *mut c_void) -> *mut c_void {
    let id = p as usize as u32;
    let t = unsafe { &*THREADS.get(id) };
    t.host.store(unsafe { libc::pthread_self() } as usize, Ordering::SeqCst);
    let c = Cpu::current();
    c.thread_id = id;
    c.guest_thread = true;
    t.cpu.store(c, Ordering::SeqCst);
    G_GUEST_THREADS.fetch_add(1, Ordering::SeqCst);
    let (func, arg) = (t.func, t.arg);
    let cp: *mut Cpu = c;
    let ret = match catch_thread_exit(|| unsafe { (*cp).call(func, &[arg]) as u32 }) {
        Ok(v) => v,
        Err(v) => v,
    };
    c.depth = 0;
    let _ = catch_thread_exit(|| run_key_destructors(unsafe { &mut *cp }));
    crate::jni::jni_thread_exit(c);
    t.cpu.store(std::ptr::null_mut(), Ordering::SeqCst);
    t.retval.store(ret, Ordering::SeqCst);
    G_GUEST_THREADS.fetch_sub(1, Ordering::SeqCst);
    std::ptr::null_mut()
}

fn new_rec(func: u32, arg: u32, detached: bool) -> *mut ThreadRec {
    Box::into_raw(Box::new(ThreadRec {
        host: AtomicUsize::new(0),
        func,
        arg,
        retval: AtomicU32::new(0),
        detached: AtomicBool::new(detached),
        cpu: AtomicPtr::new(std::ptr::null_mut()),
    }))
}

/// Guest pthread_t of the calling thread.
pub fn self_id() -> u32 {
    let c = Cpu::current();
    if c.thread_id == 0 {
        let t = new_rec(0, 0, true);
        unsafe {
            (*t).host.store(libc::pthread_self() as usize, Ordering::SeqCst);
            (*t).cpu.store(c, Ordering::SeqCst);
        }
        c.thread_id = THREADS.put(t);
    }
    c.thread_id
}

// Guest pthread_attr_t (bionic, 32-bit): flags, stack_base, stack_size, guard_size, sched_policy, sched_priority
#[repr(C)]
struct GAttr {
    flags: u32,
    stack_base: u32,
    stack_size: u32,
    guard_size: u32,
    sched_policy: i32,
    sched_priority: i32,
}

static ONCE_MU: RecMutex = RecMutex::new();

macro_rules! r {
    ($name:expr, |$($a:ident : $t:ty),*| -> $ret:ty $body:block) => {
        reg($name, move |$($a: $t),*| -> $ret { unsafe { $body } })
    };
    ($name:expr, |$($a:ident : $t:ty),*| $body:block) => {
        reg($name, move |$($a: $t),*| -> () { unsafe { $body } })
    };
}

pub fn register_pthread_thunks() {
    // ---- threads ----
    reg_raw("pthread_create", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (out, attr, func, arg) = (a.next32(), a.next32(), a.next32(), a.next32());
        let detached = attr != 0 && (rd::<u32>(attr) & 1) != 0;
        let t = new_rec(func, arg, detached);
        let id = THREADS.put(t);
        let mut ha: libc::pthread_attr_t = std::mem::zeroed();
        libc::pthread_attr_init(&mut ha);
        libc::pthread_attr_setstacksize(&mut ha, 1024 * 1024); // host side only runs the emulator loop
        if detached {
            libc::pthread_attr_setdetachstate(&mut ha, libc::PTHREAD_CREATE_DETACHED);
        }
        if out != 0 {
            wr::<u32>(out, id);
        }
        let mut host: libc::pthread_t = std::mem::zeroed();
        // The thread reads its record by id; the host handle is stored for join/detach/kill.
        let r = libc::pthread_create(&mut host, &ha, thread_main, id as usize as *mut c_void);
        libc::pthread_attr_destroy(&mut ha);
        if r != 0 {
            THREADS.take(id);
        } else {
            (*t).host.store(host as usize, Ordering::SeqCst);
        }
        c.set_ret32(r as u32);
    });
    reg("pthread_self", || -> u32 { self_id() });
    reg("pthread_equal", |x: u32, y: u32| -> i32 { (x == y) as i32 });
    r!("pthread_join", |id: u32, ret: *mut u32| -> i32 {
        let t = THREADS.get(id);
        if t.is_null() {
            return libc::ESRCH;
        }
        let host = (*t).host.load(Ordering::SeqCst) as libc::pthread_t;
        if libc::pthread_equal(host, libc::pthread_self()) != 0 {
            // A thread joining itself (seen when a network callback destroys the object that owns its
            // thread). Android of the time did not report this; today it is EDEADLK, which such games
            // turn into an uncaught C++ exception. Treat it as a detach and let the thread finish.
            static LOGGED: LogLimit = LogLimit::new();
            if LOGGED.below(4) {
                logw!("pthread_join: thread {} joins itself, treated as detach", id);
            }
            libc::pthread_detach(host);
            (*t).detached.store(true, Ordering::SeqCst);
            if !ret.is_null() {
                *ret = 0;
            }
            return 0;
        }
        let _blocking = BlockingCall::new();
        let r = libc::pthread_join(host, std::ptr::null_mut());
        if r == 0 {
            if !ret.is_null() {
                *ret = (*t).retval.load(Ordering::SeqCst);
            }
            THREADS.take(id);
            drop(Box::from_raw(t));
        }
        r
    });
    r!("pthread_detach", |id: u32| -> i32 {
        let t = THREADS.get(id);
        if t.is_null() { libc::ESRCH } else { libc::pthread_detach((*t).host.load(Ordering::SeqCst) as libc::pthread_t) }
    });
    reg_raw("pthread_exit", |c| {
        let v = ArgReader::new(c).next32();
        thread_exit(v);
    });
    r!("pthread_kill", |id: u32, sig: i32| -> i32 {
        let t = THREADS.get(id);
        if t.is_null() {
            return libc::ESRCH;
        }
        crate::signals::guest_signal_send((*t).cpu.load(Ordering::SeqCst), (*t).host.load(Ordering::SeqCst) as libc::pthread_t, sig)
    });
    reg("pthread_setname_np", |_t: u32, _n: u32| -> i32 { 0 });
    r!("pthread_getname_np", |_t: u32, b: *mut c_char, n: usize| -> i32 {
        if n != 0 {
            *b = 0;
        }
        0
    });
    reg("pthread_setschedparam", |_t: u32, _p: i32, _s: u32| -> i32 { 0 });
    r!("pthread_getschedparam", |_t: u32, pol: *mut i32, param: *mut i32| -> i32 {
        if !pol.is_null() {
            *pol = 0;
        }
        if !param.is_null() {
            *param = 0;
        }
        0
    });
    reg("pthread_setcancelstate", |_s: i32, _o: u32| -> i32 { 0 });
    reg("pthread_setcanceltype", |_s: i32, _o: u32| -> i32 { 0 });
    reg("pthread_cancel", |_t: u32| -> i32 { 0 });
    reg("pthread_testcancel", || {});
    r!("pthread_getcpuclockid", |_t: u32, clk: *mut i32| -> i32 {
        *clk = libc::CLOCK_THREAD_CPUTIME_ID;
        0
    });
    reg("pthread_gettid_np", |_t: u32| -> i32 { crate::util::gettid() });
    reg("__pthread_cleanup_push", |_c: u32, _f: u32, _a: u32| {});
    reg("__pthread_cleanup_pop", |_c: u32, _e: i32| {});

    // ---- attributes ----
    r!("pthread_attr_init", |at: *mut GAttr| -> i32 {
        std::ptr::write_bytes(at, 0, 1);
        (*at).stack_size = 1024 * 1024;
        (*at).guard_size = 4096;
        0
    });
    reg("pthread_attr_destroy", |_at: u32| -> i32 { 0 });
    r!("pthread_attr_setdetachstate", |at: *mut GAttr, st: i32| -> i32 {
        (*at).flags = ((*at).flags & !1) | (st != 0) as u32;
        0
    });
    r!("pthread_attr_getdetachstate", |at: *const GAttr, st: *mut i32| -> i32 {
        *st = ((*at).flags & 1) as i32;
        0
    });
    r!("pthread_attr_setstacksize", |at: *mut GAttr, sz: u32| -> i32 {
        (*at).stack_size = sz;
        0
    });
    r!("pthread_attr_getstacksize", |at: *const GAttr, sz: *mut u32| -> i32 {
        *sz = (*at).stack_size;
        0
    });
    r!("pthread_attr_setstack", |at: *mut GAttr, base: u32, sz: u32| -> i32 {
        (*at).stack_base = base;
        (*at).stack_size = sz;
        0
    });
    r!("pthread_attr_getstack", |_at: u32, base: *mut u32, sz: *mut u32| -> i32 {
        let c = Cpu::current();
        *base = c.stack_base;
        *sz = c.stack_size;
        0
    });
    r!("pthread_attr_setguardsize", |at: *mut GAttr, sz: u32| -> i32 {
        (*at).guard_size = sz;
        0
    });
    r!("pthread_attr_getguardsize", |at: *const GAttr, sz: *mut u32| -> i32 {
        *sz = (*at).guard_size;
        0
    });
    r!("pthread_attr_setschedpolicy", |at: *mut GAttr, p: i32| -> i32 {
        (*at).sched_policy = p;
        0
    });
    r!("pthread_attr_getschedpolicy", |at: *const GAttr, p: *mut i32| -> i32 {
        *p = (*at).sched_policy;
        0
    });
    r!("pthread_attr_setschedparam", |at: *mut GAttr, p: *const i32| -> i32 {
        (*at).sched_priority = *p;
        0
    });
    r!("pthread_attr_getschedparam", |at: *const GAttr, p: *mut i32| -> i32 {
        *p = (*at).sched_priority;
        0
    });
    reg("pthread_attr_setscope", |_at: u32, _s: i32| -> i32 { 0 });
    reg("pthread_attr_setinheritsched", |_at: u32, _s: i32| -> i32 { 0 });
    r!("pthread_getattr_np", |_t: u32, at: *mut GAttr| -> i32 {
        let c = Cpu::current();
        std::ptr::write_bytes(at, 0, 1);
        (*at).stack_base = c.stack_base;
        (*at).stack_size = c.stack_size;
        0
    });

    // ---- mutexes ----
    r!("pthread_mutex_init", |m: u32, attr: *const u32| -> i32 {
        let ty = if attr.is_null() { 0 } else { *attr & 3 };
        wr::<u32>(m, ty << 14);
        0
    });
    r!("pthread_mutex_destroy", |m: u32| -> i32 {
        destroy_object(&MUTEXES, m, |h| {
            libc::pthread_mutex_destroy(h);
        });
        0
    });
    r!("pthread_mutex_lock", |m: u32| -> i32 {
        let h = get_mutex(m);
        let r = libc::pthread_mutex_trylock(h);
        if r != libc::EBUSY {
            return r;
        }
        let _blocking = BlockingCall::new();
        libc::pthread_mutex_lock(h)
    });
    r!("pthread_mutex_trylock", |m: u32| -> i32 { libc::pthread_mutex_trylock(get_mutex(m)) });
    r!("pthread_mutex_unlock", |m: u32| -> i32 { libc::pthread_mutex_unlock(get_mutex(m)) });
    r!("pthread_mutex_timedlock", |m: u32, ts: *const GTimespec| -> i32 {
        let h = abs_from_guest(ts);
        libc::pthread_mutex_timedlock(get_mutex(m), &h)
    });
    r!("pthread_mutex_lock_timeout_np", |m: u32, ms: u32| -> i32 {
        let h = realtime_after(ms_rel(ms));
        libc::pthread_mutex_timedlock(get_mutex(m), &h)
    });
    r!("pthread_mutexattr_init", |at: *mut u32| -> i32 {
        *at = 0;
        0
    });
    reg("pthread_mutexattr_destroy", |_at: u32| -> i32 { 0 });
    r!("pthread_mutexattr_settype", |at: *mut u32, ty: i32| -> i32 {
        *at = (*at & !3) | (ty as u32 & 3);
        0
    });
    r!("pthread_mutexattr_gettype", |at: *const u32, ty: *mut i32| -> i32 {
        *ty = (*at & 3) as i32;
        0
    });
    reg("pthread_mutexattr_setpshared", |_at: u32, _v: i32| -> i32 { 0 });
    r!("pthread_mutexattr_getpshared", |_at: u32, v: *mut i32| -> i32 {
        *v = 0;
        0
    });

    // ---- condition variables ----
    r!("pthread_cond_init", |cv: u32, attr: *const u32| -> i32 {
        wr::<u32>(cv, if !attr.is_null() && (*attr & 2) != 0 { 2 } else { 0 });
        0
    });
    r!("pthread_cond_destroy", |cv: u32| -> i32 {
        destroy_object(&CONDS, cv, |h| {
            libc::pthread_cond_destroy(h);
        });
        0
    });
    r!("pthread_cond_signal", |cv: u32| -> i32 { libc::pthread_cond_signal(get_cond(cv)) });
    r!("pthread_cond_broadcast", |cv: u32| -> i32 { libc::pthread_cond_broadcast(get_cond(cv)) });
    r!("pthread_cond_wait", |cv: u32, m: u32| -> i32 {
        let _blocking = BlockingCall::new();
        libc::pthread_cond_wait(get_cond(cv), get_mutex(m))
    });
    r!("pthread_cond_timedwait", |cv: u32, m: u32, ts: *const GTimespec| -> i32 {
        let _blocking = BlockingCall::new();
        let h = abs_from_guest(ts);
        libc::pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h)
    });
    for n in ["pthread_cond_timedwait_monotonic_np", "pthread_cond_timedwait_monotonic"] {
        r!(n, |cv: u32, m: u32, ts: *const GTimespec| -> i32 {
            // Only valid for condition variables that use the realtime clock (the default).
            let _blocking = BlockingCall::new();
            let h = mono_to_real(ts.read_unaligned());
            libc::pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h)
        });
    }
    r!("pthread_cond_timedwait_relative_np", |cv: u32, m: u32, rel: *const GTimespec| -> i32 {
        let _blocking = BlockingCall::new();
        let h = realtime_after(rel.read_unaligned());
        libc::pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h)
    });
    r!("pthread_cond_timeout_np", |cv: u32, m: u32, ms: u32| -> i32 {
        let _blocking = BlockingCall::new();
        let h = realtime_after(ms_rel(ms));
        libc::pthread_cond_timedwait(get_cond(cv), get_mutex(m), &h)
    });
    r!("pthread_condattr_init", |at: *mut u32| -> i32 {
        *at = 0;
        0
    });
    reg("pthread_condattr_destroy", |_at: u32| -> i32 { 0 });
    r!("pthread_condattr_setclock", |at: *mut u32, clk: i32| -> i32 {
        *at = (*at & !2) | if clk == libc::CLOCK_MONOTONIC { 2 } else { 0 };
        0
    });
    r!("pthread_condattr_getclock", |at: *const u32, clk: *mut i32| -> i32 {
        *clk = if (*at & 2) != 0 { libc::CLOCK_MONOTONIC } else { libc::CLOCK_REALTIME };
        0
    });
    reg("pthread_condattr_setpshared", |_at: u32, _v: i32| -> i32 { 0 });

    // ---- rwlocks ----
    r!("pthread_rwlock_init", |l: u32, _attr: u32| -> i32 {
        wr::<u32>(l, 0);
        0
    });
    r!("pthread_rwlock_destroy", |l: u32| -> i32 {
        destroy_object(&RWLOCKS, l, |h| {
            libc::pthread_rwlock_destroy(h);
        });
        0
    });
    r!("pthread_rwlock_rdlock", |l: u32| -> i32 {
        let h = get_rwlock(l);
        if libc::pthread_rwlock_tryrdlock(h) == 0 {
            return 0;
        }
        let _blocking = BlockingCall::new();
        libc::pthread_rwlock_rdlock(h)
    });
    r!("pthread_rwlock_tryrdlock", |l: u32| -> i32 { libc::pthread_rwlock_tryrdlock(get_rwlock(l)) });
    r!("pthread_rwlock_wrlock", |l: u32| -> i32 {
        let h = get_rwlock(l);
        if libc::pthread_rwlock_trywrlock(h) == 0 {
            return 0;
        }
        let _blocking = BlockingCall::new();
        libc::pthread_rwlock_wrlock(h)
    });
    r!("pthread_rwlock_trywrlock", |l: u32| -> i32 { libc::pthread_rwlock_trywrlock(get_rwlock(l)) });
    r!("pthread_rwlock_unlock", |l: u32| -> i32 { libc::pthread_rwlock_unlock(get_rwlock(l)) });
    r!("pthread_rwlockattr_init", |at: *mut u32| -> i32 {
        *at = 0;
        0
    });
    reg("pthread_rwlockattr_destroy", |_at: u32| -> i32 { 0 });

    // ---- once ----
    reg_raw("pthread_once", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (ctl, func) = (a.next32(), a.next32());
        let _g = ONCE_MU.lock();
        if rd::<i32>(ctl) == 0 {
            wr::<i32>(ctl, 1);
            c.call(func, &[]);
            wr::<i32>(ctl, 2);
        }
        c.set_ret32(0);
    });

    // ---- thread-specific data ----
    r!("pthread_key_create", |key: *mut i32, dtor: u32| -> i32 {
        let mut keys = lock(&KEYS);
        for k in 1..MAX_KEYS {
            if !keys[k].used {
                keys[k] = KeySlot { used: true, dtor };
                *key = k as i32;
                return 0;
            }
        }
        libc::EAGAIN
    });
    reg("pthread_key_delete", |key: i32| -> i32 {
        if key <= 0 || key >= MAX_KEYS as i32 {
            return libc::EINVAL;
        }
        lock(&KEYS)[key as usize] = KeySlot { used: false, dtor: 0 };
        0
    });
    reg("pthread_getspecific", |key: i32| -> u32 {
        if key <= 0 || key >= MAX_KEYS as i32 {
            return 0;
        }
        Cpu::current().tls_values[key as usize]
    });
    reg("pthread_setspecific", |key: i32, value: u32| -> i32 {
        if key <= 0 || key >= MAX_KEYS as i32 {
            return libc::EINVAL;
        }
        Cpu::current().tls_values[key as usize] = value;
        0
    });

    // ---- semaphores ----
    r!("sem_init", |s: u32, _shared: i32, value: u32| -> i32 {
        wr::<u32>(s, 0);
        get_sem(s, true, value);
        0
    });
    r!("sem_destroy", |s: u32| -> i32 {
        destroy_object(&SEMS, s, |h| {
            libc::sem_destroy(h);
        });
        0
    });
    r!("sem_wait", |s: u32| -> i32 {
        let h = get_sem(s, false, 0);
        if libc::sem_trywait(h) == 0 {
            return 0;
        }
        let _blocking = BlockingCall::new();
        libc::sem_wait(h)
    });
    r!("sem_trywait", |s: u32| -> i32 { libc::sem_trywait(get_sem(s, false, 0)) });
    r!("sem_post", |s: u32| -> i32 { libc::sem_post(get_sem(s, false, 0)) });
    r!("sem_timedwait", |s: u32, ts: *const GTimespec| -> i32 {
        let _blocking = BlockingCall::new();
        let h = abs_from_guest(ts);
        libc::sem_timedwait(get_sem(s, false, 0), &h)
    });
    r!("sem_getvalue", |s: u32, out: *mut i32| -> i32 { libc::sem_getvalue(get_sem(s, false, 0), out) });

    // ---- C++ static initialisation guards (32-bit guard word, bit 0 = initialised) ----
    r!("__cxa_guard_acquire", |guard: *mut u32| -> i32 {
        if *guard & 1 != 0 {
            return 0;
        }
        ONCE_MU.lock_raw();
        if *guard & 1 != 0 {
            ONCE_MU.unlock_raw();
            return 0;
        }
        1 // lock stays held until release/abort
    });
    r!("__cxa_guard_release", |guard: *mut u32| {
        *guard |= 1;
        ONCE_MU.unlock_raw();
    });
    reg("__cxa_guard_abort", |_guard: u32| ONCE_MU.unlock_raw());
}
