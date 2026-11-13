//! Argument marshalling between the 32-bit guest ABI (AAPCS, softfp) and host functions,
//! plus the registry of host implementations of guest imports.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::collections::HashMap;
use std::sync::{Mutex, Once};

use crate::cpu::{cpu_global_init, trap_create, Cpu, TrapFn};
use crate::mem::{g2h, h2g, rd};
use crate::util::lock;

/// Reads successive arguments of a guest call.
#[derive(Clone, Copy)]
pub struct ArgReader {
    regs: *const u32,
    reg: usize, // next core register (0..4)
    stack: u32, // offset into the stack argument area
}

impl ArgReader {
    #[inline]
    pub fn new(c: &Cpu) -> Self {
        ArgReader { regs: c.regs_ptr(), reg: 0, stack: 0 }
    }
    #[inline]
    pub fn next32(&mut self) -> u32 {
        unsafe {
            if self.reg < 4 {
                let v = *self.regs.add(self.reg);
                self.reg += 1;
                return v;
            }
            let v: u32 = rd((*self.regs.add(13)).wrapping_add(self.stack));
            self.stack += 4;
            v
        }
    }
    #[inline]
    pub fn next64(&mut self) -> u64 {
        unsafe {
            if self.reg < 4 {
                self.reg = (self.reg + 1) & !1;
            }
            if self.reg < 4 {
                let v = *self.regs.add(self.reg) as u64 | ((*self.regs.add(self.reg + 1) as u64) << 32);
                self.reg += 2;
                return v;
            }
            self.stack = (self.stack + 7) & !7;
            let v: u64 = rd((*self.regs.add(13)).wrapping_add(self.stack));
            self.stack += 8;
            v
        }
    }
    #[inline]
    pub fn i32(&mut self) -> i32 {
        self.next32() as i32
    }
    /// Next argument as a host pointer.
    #[inline]
    pub fn ptr<T>(&mut self) -> *mut T {
        g2h::<T>(self.next32())
    }
    #[inline]
    pub fn f64(&mut self) -> f64 {
        f64::from_bits(self.next64())
    }
    #[inline]
    pub fn f32(&mut self) -> f32 {
        f32::from_bits(self.next32())
    }
}

/// A source of variadic arguments: either the rest of a call's arguments or a guest va_list.
pub struct VaSource {
    ar: Option<ArgReader>,
    ap: u32,
}

impl VaSource {
    pub fn from_args(a: &ArgReader) -> Self {
        VaSource { ar: Some(*a), ap: 0 }
    }
    pub fn from_va_list(ap: u32) -> Self {
        VaSource { ar: None, ap }
    }
    pub fn next32(&mut self) -> u32 {
        if let Some(a) = self.ar.as_mut() {
            return a.next32();
        }
        let v: u32 = unsafe { rd(self.ap) };
        self.ap += 4;
        v
    }
    pub fn next64(&mut self) -> u64 {
        if let Some(a) = self.ar.as_mut() {
            return a.next64();
        }
        self.ap = (self.ap + 7) & !7;
        let v: u64 = unsafe { rd(self.ap) };
        self.ap += 8;
        v
    }
}

/// A type that can be read from the guest's argument registers / stack.
/// In the guest "long", size_t and pointers are 32 bits wide: use usize/isize for them
/// (zero / sign extended), and u64/i64 only for true 64-bit quantities (long long).
pub trait GArg: Sized {
    fn get(a: &mut ArgReader) -> Self;
}
/// A type that can be returned to the guest (r0, or r0:r1 for 64-bit values).
pub trait GRet {
    fn put(self, c: &mut Cpu);
}

macro_rules! small_int {
    ($($t:ty),*) => {$(
        impl GArg for $t {
            #[inline] fn get(a: &mut ArgReader) -> Self { a.next32() as $t }
        }
        impl GRet for $t {
            #[inline] fn put(self, c: &mut Cpu) { c.set_ret32(self as i64 as u32) }
        }
    )*};
}
small_int!(u8, i8, u16, i16, u32, i32);

impl GArg for bool {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        a.next32() != 0
    }
}
impl GRet for bool {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret32(self as u32)
    }
}
impl GRet for () {
    #[inline]
    fn put(self, _c: &mut Cpu) {}
}
impl GArg for usize {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        a.next32() as usize
    }
}
impl GRet for usize {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret32(self as u32)
    }
}
impl GArg for isize {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        a.next32() as i32 as isize
    }
}
impl GRet for isize {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret32(self as u32)
    }
}
impl GArg for u64 {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        a.next64()
    }
}
impl GRet for u64 {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret64(self)
    }
}
impl GArg for i64 {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        a.next64() as i64
    }
}
impl GRet for i64 {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret64(self as u64)
    }
}
impl GArg for f32 {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        f32::from_bits(a.next32())
    }
}
impl GRet for f32 {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret32(self.to_bits())
    }
}
impl GArg for f64 {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        f64::from_bits(a.next64())
    }
}
impl GRet for f64 {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret64(self.to_bits())
    }
}
impl<T> GArg for *mut T {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        g2h::<T>(a.next32())
    }
}
impl<T> GRet for *mut T {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret32(h2g(self))
    }
}
impl<T> GArg for *const T {
    #[inline]
    fn get(a: &mut ArgReader) -> Self {
        g2h::<T>(a.next32()) as *const T
    }
}
impl<T> GRet for *const T {
    #[inline]
    fn put(self, c: &mut Cpu) {
        c.set_ret32(h2g(self))
    }
}

/// Converts a typed host function into a trap handler that marshals its arguments and result.
pub trait IntoThunk<M> {
    fn into_thunk(self) -> Box<TrapFn>;
}

macro_rules! impl_into_thunk {
    ($($a:ident),*) => {
        #[allow(non_snake_case, unused_variables, unused_mut)]
        impl<Func, Ret, $($a),*> IntoThunk<fn($($a),*) -> Ret> for Func
        where
            Func: Fn($($a),*) -> Ret + Send + Sync + 'static,
            Ret: GRet,
            $($a: GArg),*
        {
            fn into_thunk(self) -> Box<TrapFn> {
                Box::new(move |c: &mut Cpu| {
                    let mut ar = ArgReader::new(c);
                    $(let $a = <$a as GArg>::get(&mut ar);)*
                    c.load_errno();
                    let rv = self($($a),*);
                    rv.put(c);
                    c.sync_errno();
                })
            }
        }
    };
}
impl_into_thunk!();
impl_into_thunk!(A1);
impl_into_thunk!(A1, A2);
impl_into_thunk!(A1, A2, A3);
impl_into_thunk!(A1, A2, A3, A4);
impl_into_thunk!(A1, A2, A3, A4, A5);
impl_into_thunk!(A1, A2, A3, A4, A5, A6);
impl_into_thunk!(A1, A2, A3, A4, A5, A6, A7);
impl_into_thunk!(A1, A2, A3, A4, A5, A6, A7, A8);
impl_into_thunk!(A1, A2, A3, A4, A5, A6, A7, A8, A9);
impl_into_thunk!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10);
impl_into_thunk!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11);
impl_into_thunk!(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12);

// ----- registry of host-implemented guest symbols -----
struct Registry {
    impls: HashMap<String, &'static TrapFn>,
    resolved: HashMap<String, u32>,
}
static REG: Mutex<Option<Registry>> = Mutex::new(None);

fn with_reg<R>(f: impl FnOnce(&mut Registry) -> R) -> R {
    let mut g = lock(&REG);
    f(g.get_or_insert_with(|| Registry { impls: HashMap::new(), resolved: HashMap::new() }))
}

pub fn thunk_register(name: &str, f: Box<TrapFn>) {
    let f: &'static TrapFn = Box::leak(f);
    with_reg(|r| {
        r.impls.insert(name.to_string(), f);
    });
}

/// Registers a typed host function under a guest symbol name.
pub fn reg<M, F: IntoThunk<M>>(name: &str, f: F) {
    thunk_register(name, f.into_thunk());
}

/// Registers the same typed host function under several names.
pub fn reg_all<M, F: IntoThunk<M> + Clone>(names: &[&str], f: F) {
    for n in names {
        reg(n, f.clone());
    }
}

/// Raw handler that does its own marshalling.
pub fn reg_raw<F: Fn(&mut Cpu) + Send + Sync + 'static>(name: &str, f: F) {
    thunk_register(
        name,
        Box::new(move |c: &mut Cpu| {
            c.load_errno();
            f(c);
            c.sync_errno();
        }),
    );
}

pub fn thunk_register_data(name: &str, guest_addr: u32) {
    with_reg(|r| {
        r.resolved.insert(name.to_string(), guest_addr);
    });
}

pub fn thunk_exists(name: &str) -> bool {
    thunks_init();
    with_reg(|r| r.impls.contains_key(name) || r.resolved.contains_key(name))
}

/// Returns the guest address for an imported symbol or 0 if the bridge does not provide it.
pub fn thunk_resolve(name: &str) -> u32 {
    thunks_init();
    with_reg(|r| {
        if let Some(&a) = r.resolved.get(name) {
            return a;
        }
        let Some(&f) = r.impls.get(name) else { return 0 };
        let addr = trap_create(name, f);
        r.resolved.insert(name.to_string(), addr);
        addr
    })
}

/// Every symbol the bridge implements, with its guest address (creates all call stubs).
pub fn thunk_resolve_all() -> Vec<(String, u32)> {
    thunks_init();
    with_reg(|r| {
        let pending: Vec<(String, &'static TrapFn)> = r.impls.iter().filter(|(k, _)| !r.resolved.contains_key(*k)).map(|(k, &f)| (k.clone(), f)).collect();
        for (k, f) in pending {
            let a = trap_create(&k, f);
            r.resolved.insert(k, a);
        }
        r.resolved.iter().filter(|(k, _)| !k.is_empty() && !k.starts_with('?')).map(|(k, &v)| (k.clone(), v)).collect()
    })
}

/// Stub for an unknown import: logs when called and returns 0.
pub fn thunk_missing(name: &str) -> u32 {
    with_reg(|r| {
        let key = format!("?{}", name);
        if let Some(&a) = r.resolved.get(&key) {
            return a;
        }
        let owned = name.to_string();
        let f: &'static TrapFn = Box::leak(Box::new(move |c: &mut Cpu| {
            loge!("UNIMPLEMENTED function called by guest: {} (returning 0)", owned);
            c.set_ret64(0);
        }));
        let addr = trap_create(name, f);
        r.resolved.insert(key, addr);
        addr
    })
}

/// Registers every host implementation (once).
pub fn thunks_init() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        if !cpu_global_init() {
            fatal!("bridge initialisation failed");
        }
        use crate::thunks::*;
        libc::register_libc_thunks();
        stdio::register_stdio_thunks();
        math::register_math_thunks();
        pthread::register_pthread_thunks();
        misc::register_misc_thunks();
        dl::register_dl_thunks();
        old_bionic::register_old_bionic_thunks();
        crate::signals::register_signal_thunks();
        android::register_android_thunks();
        gles::register_gles_thunks();
        misc::register_data_symbols();
        zlib::register_zlib_thunks();
        posix_extra::register_posix_extra_thunks();
    });
}
