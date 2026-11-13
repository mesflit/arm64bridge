//! Bridges JNI between the 64-bit host runtime and 32-bit guest libraries.
// SPDX-License-Identifier: GPL-2.0-or-later
#![allow(non_snake_case)]
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_void, CString};
use std::os::raw::c_char;
use std::sync::atomic::{AtomicPtr, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, Once, OnceLock};

use jni_sys::*;

use crate::cpu::{trap_create, Cpu, TrapFn};
use crate::elf::{loader_find, loader_load, ModRef};
use crate::guest_types::LogLimit;
use crate::mem::*;
use crate::thunk::{thunks_init, ArgReader, GArg, GRet, VaSource};
use crate::trampoline::{tramp_address, GPR_COUNT, MAX_TRAMPOLINES};
use crate::util::{cstr, cstring, lock};

/// Calls a function of the host JNIEnv: `jni!(env, FindClass, name)`.
#[macro_export]
macro_rules! jni {
    ($env:expr, $name:ident $(, $arg:expr)* $(,)?) => {
        ((**$env).$name.expect(stringify!($name)))($env $(, $arg)*)
    };
}

static G_VM: AtomicPtr<JavaVM> = AtomicPtr::new(std::ptr::null_mut());
static G_GUEST_VM: AtomicU32 = AtomicU32::new(0); // guest JavaVM (pointer to a pointer to the invoke table)
static G_GUEST_IFACE: AtomicU32 = AtomicU32::new(0); // guest JNINativeInterface table
const IFACE_SLOTS: u32 = 240;

// ---- reference tokens ------------------------------------------------------
const LOCAL_TAG: u32 = 0x4000_0000;
const GLOBAL_TAG: u32 = 0x8000_0000;
const TAG_MASK: u32 = 0xC000_0000;

static GLOBALS: Mutex<(Vec<usize>, Vec<u32>)> = Mutex::new((Vec::new(), Vec::new())); // references, free indices

fn new_global_token(o: jobject) -> u32 {
    if o.is_null() {
        return 0;
    }
    let mut g = lock(&GLOBALS);
    let idx = match g.1.pop() {
        Some(i) => {
            g.0[i as usize] = o as usize;
            i
        }
        None => {
            g.0.push(o as usize);
            g.0.len() as u32 - 1
        }
    };
    GLOBAL_TAG | idx
}

fn take_global_token(tok: u32) -> jobject {
    let mut g = lock(&GLOBALS);
    let idx = tok & !TAG_MASK;
    if (tok & TAG_MASK) != GLOBAL_TAG || idx as usize >= g.0.len() {
        return std::ptr::null_mut();
    }
    let o = std::mem::take(&mut g.0[idx as usize]);
    if o != 0 {
        g.1.push(idx);
    }
    o as jobject
}

// ---- method / field IDs ----------------------------------------------------
struct IdInfo {
    host: usize,
    shorty: OnceLock<String>, // unset for fields
}
struct Ids {
    list: Vec<&'static IdInfo>,
    lookup: HashMap<usize, u32>,
}
static IDS: Mutex<Option<Ids>> = Mutex::new(None);

fn id_token(host: *mut c_void, shorty: &str) -> u32 {
    if host.is_null() {
        return 0;
    }
    let mut g = lock(&IDS);
    let ids = g.get_or_insert_with(|| Ids { list: Vec::new(), lookup: HashMap::new() });
    if let Some(&tok) = ids.lookup.get(&(host as usize)) {
        if !shorty.is_empty() {
            let _ = ids.list[tok as usize - 1].shorty.set(shorty.to_string());
        }
        return tok;
    }
    let info: &'static IdInfo = Box::leak(Box::new(IdInfo { host: host as usize, shorty: OnceLock::new() }));
    if !shorty.is_empty() {
        let _ = info.shorty.set(shorty.to_string());
    }
    ids.list.push(info);
    let tok = ids.list.len() as u32;
    ids.lookup.insert(host as usize, tok);
    tok
}

fn id_info(tok: u32) -> Option<&'static IdInfo> {
    let g = lock(&IDS);
    let found = g.as_ref().and_then(|ids| if tok == 0 { None } else { ids.list.get(tok as usize - 1).copied() });
    if found.is_none() {
        loge!("guest passed an invalid JNI method/field ID 0x{:x}", tok);
    }
    found
}

pub fn jni_host_vm() -> *mut JavaVM {
    G_VM.load(Ordering::Relaxed)
}

unsafe fn vm_env(version: jint) -> (jint, *mut JNIEnv) {
    let vm = jni_host_vm();
    let mut env: *mut c_void = std::ptr::null_mut();
    let rc = ((**vm).GetEnv.expect("GetEnv"))(vm, &mut env, version);
    (rc, env as *mut JNIEnv)
}

unsafe fn delete_locals(c: &mut Cpu, env: *mut JNIEnv) {
    for &o in &c.locals {
        if !o.is_null() {
            jni!(env, DeleteLocalRef, o as jobject);
        }
    }
    c.locals.clear();
    c.local_serials.clear();
}

/// Called when a guest thread ends: detaches it from the JVM if it is still attached.
pub fn jni_thread_exit(c: &mut Cpu) {
    let vm = jni_host_vm();
    if vm.is_null() {
        return;
    }
    unsafe {
        let (rc, env) = vm_env(JNI_VERSION_1_6);
        if rc != JNI_OK || env.is_null() {
            return;
        }
        delete_locals(c, env);
        for &g in c.kept_last.values() {
            jni!(env, DeleteGlobalRef, g as jobject);
        }
        c.kept_last.clear();
        c.kept_refs.clear();
        c.kept_unique.clear();
        c.kept_alias.clear();
        ((**vm).DetachCurrentThread.expect("DetachCurrentThread"))(vm);
        c.host_env = std::ptr::null_mut();
    }
}

/// Host JNIEnv of the calling thread (attaches lazily through GetEnv).
pub fn jni_host_env(c: &mut Cpu) -> *mut JNIEnv {
    if c.host_env.is_null() && !jni_host_vm().is_null() {
        let (rc, env) = unsafe { vm_env(JNI_VERSION_1_6) };
        if rc == JNI_OK {
            c.host_env = env as *mut c_void;
        }
    }
    if c.host_env.is_null() {
        fatal!("guest code used JNI on a thread that is not attached to the Java VM");
    }
    c.host_env as *mut JNIEnv
}

const MAX_KEPT_REFS: usize = 4096; // distinct argument objects kept alive per thread
const MAX_KEPT_ALIASES: usize = 1024;

// Index of a local token in the thread's table.
fn local_index(c: &Cpu, serial: u32) -> Option<usize> {
    c.local_serials.binary_search(&serial).ok()
}

pub fn jni_new_local(c: &mut Cpu, obj: jobject) -> u32 {
    if obj.is_null() {
        return 0;
    }
    let mut serial = c.next_local_serial;
    c.next_local_serial += 1;
    if c.next_local_serial >= LOCAL_TAG {
        // wrapped: start over (stale tokens become invalid)
        c.locals.clear();
        c.local_serials.clear();
        serial = 1;
        c.next_local_serial = 2;
    }
    c.locals.push(obj as *mut c_void);
    c.local_serials.push(serial);
    LOCAL_TAG | serial
}

pub fn jni_to_host(c: &mut Cpu, tok: u32) -> jobject {
    if tok == 0 {
        return std::ptr::null_mut();
    }
    let idx = tok & !TAG_MASK;
    if (tok & TAG_MASK) == LOCAL_TAG {
        if let Some(i) = local_index(c, idx) {
            if !c.locals[i].is_null() {
                return c.locals[i] as jobject;
            }
        }
        if let Some(&kept) = c.kept_refs.get(&idx) {
            return kept as jobject;
        }
    } else if (tok & TAG_MASK) == GLOBAL_TAG {
        if let Some(&o) = lock(&GLOBALS).0.get(idx as usize) {
            return o as jobject;
        }
    }
    loge!("guest passed an invalid JNI reference 0x{:08x}", tok);
    std::ptr::null_mut()
}

/// Scope of one Java -> guest transition: sets the host env and releases the
/// local reference tokens created inside it.
pub struct JniFrame {
    c: *mut Cpu,
    base: usize,
    saved_env: *mut c_void,
    /// identifies the native method (for recognising the same argument object again)
    keep_key: u32,
    /// leading references of this frame (the call's arguments) to keep alive afterwards
    keep: usize,
}

impl JniFrame {
    pub fn new(c: &mut Cpu, env: *mut JNIEnv) -> JniFrame {
        let f = JniFrame { c, base: c.locals.len(), saved_env: c.host_env, keep_key: 0, keep: 0 };
        c.host_env = env as *mut c_void;
        f
    }
}

impl Drop for JniFrame {
    fn drop(&mut self) {
        let c = unsafe { &mut *self.c };
        let env = c.host_env as *mut JNIEnv;
        unsafe {
            let mut i = self.base;
            while !env.is_null() && i < self.base + self.keep && i < c.locals.len() {
                let (local, serial) = (c.locals[i] as jobject, c.local_serials[i]);
                let key = ((self.keep_key as u64) << 8) | (i - self.base) as u64;
                i += 1;
                if local.is_null() {
                    continue;
                }
                if let Some(&last) = c.kept_last.get(&key) {
                    if jni!(env, IsSameObject, local, last as jobject) != 0 {
                        // The same object as in the previous call (typically "this"): share its reference.
                        c.kept_refs.insert(serial, last);
                        c.kept_alias.push_back(serial);
                        continue;
                    }
                }
                let g = jni!(env, NewGlobalRef, local);
                if g.is_null() {
                    continue;
                }
                c.kept_refs.insert(serial, g as *mut c_void);
                c.kept_unique.push_back(serial);
                c.kept_last.insert(key, g as *mut c_void);
            }
            while c.kept_alias.len() > MAX_KEPT_ALIASES {
                if let Some(a) = c.kept_alias.pop_front() {
                    c.kept_refs.remove(&a);
                }
            }
            while !env.is_null() && c.kept_unique.len() > MAX_KEPT_REFS {
                let Some(front) = c.kept_unique.pop_front() else { break };
                let Some(g) = c.kept_refs.remove(&front) else { continue };
                for alias in c.kept_alias.iter() {
                    if c.kept_refs.get(alias) == Some(&g) {
                        c.kept_refs.remove(alias);
                    }
                }
                c.kept_last.retain(|_, v| *v != g);
                jni!(env, DeleteGlobalRef, g as jobject);
            }
        }
        if c.locals.len() > self.base {
            c.locals.truncate(self.base);
            c.local_serials.truncate(self.base);
        }
        c.host_env = self.saved_env;
    }
}

/// "(ILjava/lang/String;[B)V" -> "VILL": return type first, one letter per argument.
pub fn jni_shorty_from_signature(sig: &str) -> String {
    let s = sig.as_bytes();
    let mut args = String::new();
    let mut ret = 'V';
    let mut p = if s.first() == Some(&b'(') { 1 } else { 0 };
    let mut in_ret = false;
    while p < s.len() {
        if s[p] == b')' {
            in_ret = true;
            p += 1;
            continue;
        }
        let mut ch = s[p] as char;
        if ch == '[' {
            while p < s.len() && s[p] == b'[' {
                p += 1;
            }
            if p < s.len() && s[p] == b'L' {
                while p < s.len() && s[p] != b';' {
                    p += 1;
                }
            }
            if p < s.len() {
                p += 1;
            }
            ch = 'L';
        } else if ch == 'L' {
            while p < s.len() && s[p] != b';' {
                p += 1;
            }
            if p < s.len() {
                p += 1;
            }
        } else {
            p += 1;
        }
        if in_ret {
            ret = ch;
            break;
        }
        args.push(ch);
    }
    format!("{}{}", ret, args)
}

// ---------------------------------------------------------------------------
// Marshalling of JNI functions: references and IDs travel as tokens, jlong is 64 bits wide.
// ---------------------------------------------------------------------------
trait JArg: Sized {
    fn get(a: &mut ArgReader, c: &mut Cpu) -> Self;
}
trait JRet {
    fn put(self, c: &mut Cpu);
}

macro_rules! plain_jni_types {
    ($($t:ty),*) => {$(
        impl JArg for $t {
            #[inline] fn get(a: &mut ArgReader, _c: &mut Cpu) -> Self { <$t as GArg>::get(a) }
        }
        impl JRet for $t {
            #[inline] fn put(self, c: &mut Cpu) { GRet::put(self, c) }
        }
    )*};
}
plain_jni_types!(
    u8, i8, u16, i16, i32, i64, f32, f64, *const u8, *mut u8, *const i8, *mut i8, *const u16, *mut u16, *const i16, *mut i16, *const i32, *mut i32,
    *const i64, *mut i64, *const f32, *mut f32, *const f64, *mut f64, *mut c_void
);
impl JRet for () {
    fn put(self, _c: &mut Cpu) {}
}
impl JArg for jobject {
    fn get(a: &mut ArgReader, c: &mut Cpu) -> Self {
        let tok = a.next32();
        jni_to_host(c, tok)
    }
}
impl JRet for jobject {
    fn put(self, c: &mut Cpu) {
        let tok = jni_new_local(c, self);
        c.set_ret32(tok)
    }
}
impl JArg for jmethodID {
    fn get(a: &mut ArgReader, _c: &mut Cpu) -> Self {
        id_info(a.next32()).map(|i| i.host).unwrap_or(0) as jmethodID
    }
}
impl JRet for jmethodID {
    fn put(self, c: &mut Cpu) {
        c.set_ret32(id_token(self as *mut c_void, ""))
    }
}
impl JArg for jfieldID {
    fn get(a: &mut ArgReader, _c: &mut Cpu) -> Self {
        id_info(a.next32()).map(|i| i.host).unwrap_or(0) as jfieldID
    }
}
impl JRet for jfieldID {
    fn put(self, c: &mut Cpu) {
        c.set_ret32(id_token(self as *mut c_void, ""))
    }
}

// A function of the host JNINativeInterface that can be called with arguments read from the guest.
trait JniFn {
    unsafe fn invoke(self, env: *mut JNIEnv, c: &mut Cpu, a: &mut ArgReader);
}
macro_rules! impl_jni_fn {
    ($($a:ident),*) => {
        impl<R: JRet, $($a: JArg),*> JniFn for unsafe extern "system" fn(*mut JNIEnv $(, $a)*) -> R {
            #[allow(unused_variables)]
            unsafe fn invoke(self, env: *mut JNIEnv, c: &mut Cpu, a: &mut ArgReader) {
                $(let $a = <$a as JArg>::get(a, c);)*
                let rv = self(env $(, $a)*);
                rv.put(c);
            }
        }
    };
}
impl_jni_fn!();
impl_jni_fn!(A1);
impl_jni_fn!(A1, A2);
impl_jni_fn!(A1, A2, A3);
impl_jni_fn!(A1, A2, A3, A4);
impl_jni_fn!(A1, A2, A3, A4, A5);

unsafe fn jstring_text(env: *mut JNIEnv, s: jstring) -> Option<String> {
    if s.is_null() {
        return None;
    }
    let chars = jni!(env, GetStringUTFChars, s, std::ptr::null_mut());
    if chars.is_null() {
        return None;
    }
    let text = cstr(chars);
    jni!(env, ReleaseStringUTFChars, s, chars);
    Some(text)
}

unsafe fn clear_exception(env: *mut JNIEnv) -> bool {
    if jni!(env, ExceptionCheck) == 0 {
        return false;
    }
    jni!(env, ExceptionClear);
    true
}

const PRIMS: [(&str, char); 9] = [("int", 'I'), ("boolean", 'Z'), ("byte", 'B'), ("char", 'C'), ("short", 'S'), ("long", 'J'), ("float", 'F'), ("double", 'D'), ("void", 'V')];

// Class.getName() of a java.lang.Class object.
unsafe fn class_name(env: *mut JNIEnv, ty: jclass, get_name: jmethodID) -> String {
    let js = jni!(env, CallObjectMethodA, ty, get_name, std::ptr::null());
    let n = jstring_text(env, js).unwrap_or_default();
    if !js.is_null() {
        jni!(env, DeleteLocalRef, js);
    }
    n
}

// The shorty of a java.lang.reflect.Method or Constructor, so that a method ID from
// FromReflectedMethod can be called like one from GetMethodID (Unity's AndroidJavaObject does this).
unsafe fn reflected_shorty(env: *mut JNIEnv, member: jobject) -> String {
    if member.is_null() {
        return String::new();
    }
    let type_char = |ty: jclass| -> char {
        let cc = jni!(env, GetObjectClass, ty);
        let get_name = jni!(env, GetMethodID, cc, c"getName".as_ptr(), c"()Ljava/lang/String;".as_ptr());
        jni!(env, DeleteLocalRef, cc);
        let n = class_name(env, ty, get_name);
        PRIMS.iter().find(|p| p.0 == n).map(|p| p.1).unwrap_or('L')
    };
    let mc = jni!(env, GetObjectClass, member);
    let get_params = jni!(env, GetMethodID, mc, c"getParameterTypes".as_ptr(), c"()[Ljava/lang/Class;".as_ptr());
    let get_ret = jni!(env, GetMethodID, mc, c"getReturnType".as_ptr(), c"()Ljava/lang/Class;".as_ptr());
    clear_exception(env); // a Constructor has no getReturnType
    jni!(env, DeleteLocalRef, mc);
    if get_params.is_null() {
        return String::new();
    }
    let mut shorty = String::from("V");
    if !get_ret.is_null() {
        let rt = jni!(env, CallObjectMethodA, member, get_ret, std::ptr::null());
        shorty = type_char(rt).to_string();
        jni!(env, DeleteLocalRef, rt);
    }
    let params = jni!(env, CallObjectMethodA, member, get_params, std::ptr::null());
    let np = if params.is_null() { 0 } else { jni!(env, GetArrayLength, params) };
    for i in 0..np {
        let pt = jni!(env, GetObjectArrayElement, params, i);
        shorty.push(type_char(pt));
        jni!(env, DeleteLocalRef, pt);
    }
    if !params.is_null() {
        jni!(env, DeleteLocalRef, params);
    }
    if clear_exception(env) {
        return String::new();
    }
    shorty
}

// ---- direct buffers outside the guest address space -----------------------------
//
// Java allocates direct ByteBuffers (ByteBuffer.allocateDirect) anywhere in the 64-bit address
// space; guest code can only use addresses below 4 GiB. Such a buffer gets a copy in guest memory
// (a mirror) that guest code works on. The mirror is filled from the Java buffer when guest code asks
// for its address, and written back when the native method that asked returns to Java, and around
// calls from guest code into Java. Unity streams its audio this way (AudioTrack and a direct buffer).
#[derive(Clone, Copy)]
struct ActiveMirror {
    host: usize,
    guest: u32,
    size: usize,
}
static MIRRORS: Mutex<Option<HashMap<usize, (u32, usize)>>> = Mutex::new(None); // host address -> (guest copy, size)
thread_local! {
    static ACTIVE_MIRRORS: RefCell<Vec<ActiveMirror>> = const { RefCell::new(Vec::new()) };
}

unsafe fn mirror_direct_buffer(env: *mut JNIEnv, buf: jobject, host: *mut c_void) -> u32 {
    let cap = jni!(env, GetDirectBufferCapacity, buf);
    if cap <= 0 || cap > (64 << 20) {
        logw!("GetDirectBufferAddress: buffer {:p} ({} bytes) lives outside the 32-bit guest address space", host, cap);
        return 0;
    }
    let size = cap as usize;
    let guest = {
        let mut g = lock(&MIRRORS);
        let map = g.get_or_insert_with(HashMap::new);
        let e = map.entry(host as usize).or_insert((0, 0));
        if e.0 != 0 && e.1 != size {
            g_free(e.0);
            e.0 = 0;
        }
        let mut fresh = false;
        if e.0 == 0 {
            *e = (g_malloc(size as u32), size);
            fresh = true;
        }
        let guest = e.0;
        let n = map.len();
        if fresh && (n <= 16 || n.is_power_of_two()) {
            logi!("direct buffer {:p} ({} bytes) outside guest memory: mirrored at 0x{:08x} ({} mirrors)", host, cap, guest, n);
        }
        guest
    };
    if guest == 0 {
        return 0;
    }
    std::ptr::copy_nonoverlapping(host as *const u8, g2h::<u8>(guest), size);
    ACTIVE_MIRRORS.with(|m| {
        let mut m = m.borrow_mut();
        if !m.iter().any(|a| a.host == host as usize) {
            m.push(ActiveMirror { host: host as usize, guest, size });
        }
    });
    guest
}

fn active_mirror_count() -> usize {
    ACTIVE_MIRRORS.with(|m| m.borrow().len())
}

// Guest -> Java copies of the mirrors this thread uses; from index [from] on.
unsafe fn flush_mirrors(from: usize) {
    ACTIVE_MIRRORS.with(|m| {
        for a in m.borrow().iter().skip(from) {
            std::ptr::copy_nonoverlapping(g2h::<u8>(a.guest), a.host as *mut u8, a.size);
        }
    });
}

// Java -> guest, after Java code ran.
unsafe fn refresh_mirrors() {
    ACTIVE_MIRRORS.with(|m| {
        for a in m.borrow().iter() {
            std::ptr::copy_nonoverlapping(a.host as *const u8, g2h::<u8>(a.guest), a.size);
        }
    });
}

// A Java exception left pending by guest code.
//
// The JNI rules allow only a few functions (ExceptionCheck, Release*, Delete*Ref...) while an exception
// is pending; ART aborts the process for any other. Old games often ignore exceptions that their
// Android version never threw: Samurai vs Zombies reads the IMEI (getDeviceId throws a
// SecurityException since Android 10) and calls FindClass right after. As an old Android would have
// let the game go on, the exception is logged and cleared before such a call.
unsafe fn clear_ignored_exception(env: *mut JNIEnv, call: &str) {
    let t = jni!(env, ExceptionOccurred);
    jni!(env, ExceptionClear);
    static LOGGED: LogLimit = LogLimit::new();
    if !LOGGED.below(20) || t.is_null() {
        if !t.is_null() {
            jni!(env, DeleteLocalRef, t);
        }
        return;
    }
    let tc = jni!(env, GetObjectClass, t);
    let to_string = jni!(env, GetMethodID, tc, c"toString".as_ptr(), c"()Ljava/lang/String;".as_ptr());
    let js = if to_string.is_null() { std::ptr::null_mut() } else { jni!(env, CallObjectMethodA, t, to_string, std::ptr::null()) };
    clear_exception(env);
    let what = jstring_text(env, js).unwrap_or_else(|| "?".into());
    if !js.is_null() {
        jni!(env, DeleteLocalRef, js);
    }
    jni!(env, DeleteLocalRef, tc);
    jni!(env, DeleteLocalRef, t);
    logw!("guest code called {} with a pending Java exception it did not handle; cleared it: {}", call, what);
}

fn allowed_with_pending_exception(name: &str) -> bool {
    ["Exception", "Release", "Delete", "MonitorExit", "PushLocalFrame", "PopLocalFrame", "Throw"].iter().any(|a| name.contains(a))
}

fn set_slot(index: usize, name: &'static str, f: Box<TrapFn>) {
    if index >= IFACE_SLOTS as usize {
        fatal!("JNI slot out of range");
    }
    let f: Box<TrapFn> = if allowed_with_pending_exception(name) {
        f
    } else {
        Box::new(move |c: &mut Cpu| unsafe {
            let env = jni_host_env(c);
            if jni!(env, ExceptionCheck) != 0 {
                clear_ignored_exception(env, name);
            }
            f(c)
        })
    };
    unsafe { wr::<u32>(G_GUEST_IFACE.load(Ordering::Relaxed) + index as u32 * 4, trap_create(name, Box::leak(f))) };
}

macro_rules! slot {
    ($name:ident) => {
        std::mem::offset_of!(JNINativeInterface_, $name) / std::mem::size_of::<usize>()
    };
}

// Straight pass-through with automatic marshalling.
macro_rules! auto {
    ($($name:ident),* $(,)?) => {$(
        set_slot(slot!($name), concat!("JNI:", stringify!($name)), Box::new(|c: &mut Cpu| unsafe {
            let env = jni_host_env(c);
            let f = (**env).$name.expect(stringify!($name));
            let mut a = ArgReader::new(c);
            a.next32(); // guest JNIEnv*
            JniFn::invoke(f, env, c, &mut a);
        }));
    )*};
}

// Hand-written slot: the body gets the Cpu, the host env and an ArgReader positioned after the JNIEnv*.
macro_rules! hand {
    ($name:ident, |$c:ident, $env:ident, $a:ident| $body:block) => {
        set_slot(slot!($name), concat!("JNI:", stringify!($name)), Box::new(|$c: &mut Cpu| unsafe {
            let $env = jni_host_env($c);
            let mut $a = ArgReader::new($c);
            $a.next32();
            let _ = &$env;
            $body
        }));
    };
}

// ---- Call<Type>Method{,V,A} families ---------------------------------------
#[derive(Clone, Copy, PartialEq, Eq)]
enum CallKind {
    Virtual,
    Nonvirtual,
    Static,
    New,
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum VaKind {
    Dots,
    VaList,
    Array,
}

// Java code may read or fill direct buffers that guest code works on through mirrors.
struct MirrorSync(bool);
impl MirrorSync {
    unsafe fn new() -> Self {
        let any = active_mirror_count() != 0;
        if any {
            flush_mirrors(0);
        }
        MirrorSync(any)
    }
}
impl Drop for MirrorSync {
    fn drop(&mut self) {
        if self.0 {
            unsafe { refresh_mirrors() };
        }
    }
}

unsafe fn call_family(c: &mut Cpu, ret: char, kind: CallKind, va: VaKind) {
    let env = jni_host_env(c);
    let mut a = ArgReader::new(c);
    a.next32();
    let tok = a.next32();
    let obj = jni_to_host(c, tok);
    let cls: jclass = if kind == CallKind::Nonvirtual {
        let t = a.next32();
        jni_to_host(c, t)
    } else {
        std::ptr::null_mut()
    };
    let info = id_info(a.next32());
    let Some((mid, sh)) = info.and_then(|i| i.shorty.get().map(|s| (i.host as jmethodID, s.as_bytes()))) else {
        loge!("JNI call with an unknown method ID");
        c.set_ret64(0);
        return;
    };
    let mut args: Vec<jvalue> = vec![jvalue { j: 0 }; sh.len()];
    if va == VaKind::Array {
        let mut p = a.next32();
        for i in 1..sh.len() {
            let v = &mut args[i - 1];
            match sh[i] {
                b'J' | b'D' => v.j = rd::<i64>(p),
                b'L' => v.l = jni_to_host(c, rd::<u32>(p)),
                _ => v.i = rd::<i32>(p),
            }
            p += 8;
        }
    } else {
        let mut src = if va == VaKind::VaList { VaSource::from_va_list(a.next32()) } else { VaSource::from_args(&a) };
        for i in 1..sh.len() {
            let v = &mut args[i - 1];
            match sh[i] {
                b'J' => v.j = src.next64() as i64,
                b'D' => v.d = f64::from_bits(src.next64()),
                b'F' => v.f = f64::from_bits(src.next64()) as f32, // promoted to double
                b'L' => v.l = jni_to_host(c, src.next32()),
                _ => v.i = src.next32() as i32,
            }
        }
    }
    let av = args.as_ptr();
    let _mirror_sync = MirrorSync::new();
    if kind == CallKind::New {
        let o = jni!(env, NewObjectA, obj, mid, av);
        let t = jni_new_local(c, o);
        c.set_ret32(t);
        return;
    }
    macro_rules! dispatch {
        ($virt:ident, $nonvirt:ident, $stat:ident) => {
            match kind {
                CallKind::Static => jni!(env, $stat, obj, mid, av),
                CallKind::Nonvirtual => jni!(env, $nonvirt, obj, cls, mid, av),
                _ => jni!(env, $virt, obj, mid, av),
            }
        };
    }
    match ret {
        'V' => dispatch!(CallVoidMethodA, CallNonvirtualVoidMethodA, CallStaticVoidMethodA),
        'L' => {
            let o = dispatch!(CallObjectMethodA, CallNonvirtualObjectMethodA, CallStaticObjectMethodA);
            let t = jni_new_local(c, o);
            c.set_ret32(t);
        }
        'Z' => c.set_ret32(dispatch!(CallBooleanMethodA, CallNonvirtualBooleanMethodA, CallStaticBooleanMethodA) as u32),
        'B' => c.set_ret32(dispatch!(CallByteMethodA, CallNonvirtualByteMethodA, CallStaticByteMethodA) as i32 as u32),
        'C' => c.set_ret32(dispatch!(CallCharMethodA, CallNonvirtualCharMethodA, CallStaticCharMethodA) as u32),
        'S' => c.set_ret32(dispatch!(CallShortMethodA, CallNonvirtualShortMethodA, CallStaticShortMethodA) as i32 as u32),
        'I' => c.set_ret32(dispatch!(CallIntMethodA, CallNonvirtualIntMethodA, CallStaticIntMethodA) as u32),
        'J' => c.set_ret64(dispatch!(CallLongMethodA, CallNonvirtualLongMethodA, CallStaticLongMethodA) as u64),
        'F' => c.set_ret32(dispatch!(CallFloatMethodA, CallNonvirtualFloatMethodA, CallStaticFloatMethodA).to_bits()),
        'D' => c.set_ret64(dispatch!(CallDoubleMethodA, CallNonvirtualDoubleMethodA, CallStaticDoubleMethodA).to_bits()),
        _ => {}
    }
}

fn call_slot(index: usize, name: &'static str, ret: char, kind: CallKind, va: VaKind) {
    set_slot(index, name, Box::new(move |c: &mut Cpu| unsafe { call_family(c, ret, kind, va) }));
}

macro_rules! call_slots {
    ($($ch:literal: $m:ident $mv:ident $ma:ident, $n:ident $nv:ident $na:ident, $s:ident $sv:ident $sa:ident;)*) => {$(
        call_slot(slot!($m), concat!("JNI:", stringify!($m)), $ch, CallKind::Virtual, VaKind::Dots);
        call_slot(slot!($mv), concat!("JNI:", stringify!($mv)), $ch, CallKind::Virtual, VaKind::VaList);
        call_slot(slot!($ma), concat!("JNI:", stringify!($ma)), $ch, CallKind::Virtual, VaKind::Array);
        call_slot(slot!($n), concat!("JNI:", stringify!($n)), $ch, CallKind::Nonvirtual, VaKind::Dots);
        call_slot(slot!($nv), concat!("JNI:", stringify!($nv)), $ch, CallKind::Nonvirtual, VaKind::VaList);
        call_slot(slot!($na), concat!("JNI:", stringify!($na)), $ch, CallKind::Nonvirtual, VaKind::Array);
        call_slot(slot!($s), concat!("JNI:", stringify!($s)), $ch, CallKind::Static, VaKind::Dots);
        call_slot(slot!($sv), concat!("JNI:", stringify!($sv)), $ch, CallKind::Static, VaKind::VaList);
        call_slot(slot!($sa), concat!("JNI:", stringify!($sa)), $ch, CallKind::Static, VaKind::Array);
    )*};
}

// ---- primitive arrays -------------------------------------------------------
// Element storage handed to the guest is a copy in guest memory.
const ARRAY_KINDS: [(&std::ffi::CStr, u32); 8] = [(c"[Z", 1), (c"[B", 1), (c"[C", 2), (c"[S", 2), (c"[I", 4), (c"[J", 8), (c"[F", 4), (c"[D", 8)];
static ARRAY_CLASSES: [AtomicUsize; 8] = [const { AtomicUsize::new(0) }; 8];

unsafe fn array_region(env: *mut JNIEnv, arr: jarray, kind: usize, len: jsize, buf: u32, set: bool) {
    macro_rules! k {
        ($get:ident, $set:ident) => {
            if set {
                jni!(env, $set, arr, 0, len, g2h(buf))
            } else {
                jni!(env, $get, arr, 0, len, g2h(buf))
            }
        };
    }
    match kind {
        0 => k!(GetBooleanArrayRegion, SetBooleanArrayRegion),
        1 => k!(GetByteArrayRegion, SetByteArrayRegion),
        2 => k!(GetCharArrayRegion, SetCharArrayRegion),
        3 => k!(GetShortArrayRegion, SetShortArrayRegion),
        4 => k!(GetIntArrayRegion, SetIntArrayRegion),
        5 => k!(GetLongArrayRegion, SetLongArrayRegion),
        6 => k!(GetFloatArrayRegion, SetFloatArrayRegion),
        _ => k!(GetDoubleArrayRegion, SetDoubleArrayRegion),
    }
}

unsafe fn array_kind_of(env: *mut JNIEnv, arr: jarray) -> usize {
    (0..8).find(|&i| jni!(env, IsInstanceOf, arr, ARRAY_CLASSES[i].load(Ordering::Relaxed) as jclass) != 0).unwrap_or(1)
}

// kind < 0: any primitive array (the Critical functions).
unsafe fn get_elements(env: *mut JNIEnv, arr: jarray, kind: i32, is_copy: u32) -> u32 {
    if arr.is_null() {
        return 0;
    }
    let kind = if kind < 0 { array_kind_of(env, arr) } else { kind as usize };
    let len = jni!(env, GetArrayLength, arr);
    let buf = g_malloc(len as u32 * ARRAY_KINDS[kind].1 + 8);
    if buf == 0 {
        return 0;
    }
    array_region(env, arr, kind, len, buf, false);
    if is_copy != 0 {
        wr::<u8>(is_copy, JNI_TRUE);
    }
    buf
}

unsafe fn release_elements(env: *mut JNIEnv, arr: jarray, kind: i32, buf: u32, mode: jint) {
    if arr.is_null() || buf == 0 {
        return;
    }
    let kind = if kind < 0 { array_kind_of(env, arr) } else { kind as usize };
    if mode != JNI_ABORT {
        array_region(env, arr, kind, jni!(env, GetArrayLength, arr), buf, true);
    }
    if mode != JNI_COMMIT {
        g_free(buf);
    }
}

// ---- native method records --------------------------------------------------
struct NativeMethod {
    guest_fn: u32,
    shorty: String,
    name: String,
    calls: AtomicU64,
    reported: AtomicU64,
}
static METHODS: [AtomicPtr<NativeMethod>; MAX_TRAMPOLINES as usize] = [const { AtomicPtr::new(std::ptr::null_mut()) }; MAX_TRAMPOLINES as usize];
static METHOD_COUNT: AtomicU32 = AtomicU32::new(0);
static METHOD_CACHE: Mutex<Option<HashMap<String, usize>>> = Mutex::new(None);

fn methods() -> impl Iterator<Item = &'static NativeMethod> {
    let n = METHOD_COUNT.load(Ordering::SeqCst) as usize;
    METHODS[..n].iter().filter_map(|m| unsafe { m.load(Ordering::Acquire).as_ref() })
}

// ---- guest JavaVM -------------------------------------------------------------
fn guest_env_for(c: &mut Cpu) -> u32 {
    if c.guest_env == 0 {
        c.guest_env = g_calloc(1, 8);
        unsafe { wr::<u32>(c.guest_env, G_GUEST_IFACE.load(Ordering::Relaxed)) };
    }
    c.guest_env
}

unsafe fn vm_get_env(c: &mut Cpu) {
    let mut a = ArgReader::new(c);
    a.next32();
    let penv = a.next32();
    let version = a.i32();
    let (rc, env) = vm_env(if version > 0 { version } else { JNI_VERSION_1_6 });
    if rc == JNI_OK {
        c.host_env = env as *mut c_void;
        if penv != 0 {
            wr::<u32>(penv, guest_env_for(c));
        }
    } else if penv != 0 {
        wr::<u32>(penv, 0);
    }
    c.set_ret32(rc as u32);
}

unsafe fn vm_attach(c: &mut Cpu, daemon: bool) {
    let mut a = ArgReader::new(c);
    a.next32();
    let (penv, gargs) = (a.next32(), a.next32());
    let mut args = JavaVMAttachArgs { version: JNI_VERSION_1_6, name: std::ptr::null_mut(), group: std::ptr::null_mut() };
    if gargs != 0 {
        args.version = rd::<i32>(gargs);
        args.name = g2h::<c_char>(rd::<u32>(gargs + 4));
        args.group = jni_to_host(c, rd::<u32>(gargs + 8));
    }
    let vm = jni_host_vm();
    let mut env: *mut c_void = std::ptr::null_mut();
    let attach = if daemon { (**vm).AttachCurrentThreadAsDaemon } else { (**vm).AttachCurrentThread }.expect("AttachCurrentThread");
    let rc = attach(vm, &mut env, &mut args as *mut JavaVMAttachArgs as *mut c_void);
    if rc == JNI_OK {
        c.host_env = env;
        c.detach_deferred = false;
        if penv != 0 {
            wr::<u32>(penv, guest_env_for(c));
        }
    }
    c.set_ret32(rc as u32);
}

unsafe fn vm_detach(c: &mut Cpu) {
    if c.guest_thread && !c.host_env.is_null() {
        // Games often attach and detach a thread around every Java call (several times per frame).
        // Attaching creates a java.lang.Thread each time, so keep guest threads attached until they
        // end, and only release what a real detach would release: the thread's local references.
        let env = c.host_env as *mut JNIEnv;
        delete_locals(c, env);
        c.detach_deferred = true;
        c.set_ret32(JNI_OK as u32);
        return;
    }
    let vm = jni_host_vm();
    let rc = ((**vm).DetachCurrentThread.expect("DetachCurrentThread"))(vm);
    c.host_env = std::ptr::null_mut();
    c.locals.clear();
    c.local_serials.clear();
    c.set_ret32(rc as u32);
}

fn raw_trap(name: &str, f: impl Fn(&mut Cpu) + Send + Sync + 'static) -> u32 {
    trap_create(name, Box::leak(Box::new(f)))
}

unsafe fn build_tables() {
    let iface = g_calloc(IFACE_SLOTS, 4);
    G_GUEST_IFACE.store(iface, Ordering::SeqCst);
    for i in 0..IFACE_SLOTS {
        wr::<u32>(
            iface + i * 4,
            raw_trap("JNI:<unsupported>", move |c| {
                loge!("guest called unsupported JNI function #{}", i);
                c.set_ret64(0);
            }),
        );
    }

    auto!(
        GetVersion, DefineClass, FindClass, FromReflectedField, ToReflectedMethod, GetSuperclass, IsAssignableFrom, ToReflectedField, Throw, ThrowNew,
        ExceptionOccurred, ExceptionDescribe, ExceptionClear, PushLocalFrame, PopLocalFrame, NewLocalRef, IsSameObject, EnsureLocalCapacity, AllocObject,
        GetObjectClass, IsInstanceOf, GetFieldID, GetStaticFieldID,
        GetObjectField, SetObjectField, GetStaticObjectField, SetStaticObjectField,
        GetBooleanField, SetBooleanField, GetStaticBooleanField, SetStaticBooleanField,
        GetByteField, SetByteField, GetStaticByteField, SetStaticByteField,
        GetCharField, SetCharField, GetStaticCharField, SetStaticCharField,
        GetShortField, SetShortField, GetStaticShortField, SetStaticShortField,
        GetIntField, SetIntField, GetStaticIntField, SetStaticIntField,
        GetLongField, SetLongField, GetStaticLongField, SetStaticLongField,
        GetFloatField, SetFloatField, GetStaticFloatField, SetStaticFloatField,
        GetDoubleField, SetDoubleField, GetStaticDoubleField, SetStaticDoubleField,
        NewString, GetStringLength, NewStringUTF, GetStringUTFLength, GetArrayLength, NewObjectArray, GetObjectArrayElement, SetObjectArrayElement,
        NewBooleanArray, GetBooleanArrayRegion, SetBooleanArrayRegion,
        NewByteArray, GetByteArrayRegion, SetByteArrayRegion,
        NewCharArray, GetCharArrayRegion, SetCharArrayRegion,
        NewShortArray, GetShortArrayRegion, SetShortArrayRegion,
        NewIntArray, GetIntArrayRegion, SetIntArrayRegion,
        NewLongArray, GetLongArrayRegion, SetLongArrayRegion,
        NewFloatArray, GetFloatArrayRegion, SetFloatArrayRegion,
        NewDoubleArray, GetDoubleArrayRegion, SetDoubleArrayRegion,
        UnregisterNatives, MonitorEnter, MonitorExit, GetStringRegion, GetStringUTFRegion, ExceptionCheck, NewDirectByteBuffer, GetDirectBufferCapacity,
    );
    hand!(FatalError, |c, env, a| {
        let msg: *const c_char = a.ptr();
        jni!(env, FatalError, msg);
    });
    hand!(GetObjectRefType, |c, env, a| {
        let t = a.next32();
        let o = jni_to_host(c, t);
        c.set_ret32(jni!(env, GetObjectRefType, o) as u32);
    });

    // -- method calls --
    call_slots! {
        'L': CallObjectMethod CallObjectMethodV CallObjectMethodA, CallNonvirtualObjectMethod CallNonvirtualObjectMethodV CallNonvirtualObjectMethodA, CallStaticObjectMethod CallStaticObjectMethodV CallStaticObjectMethodA;
        'Z': CallBooleanMethod CallBooleanMethodV CallBooleanMethodA, CallNonvirtualBooleanMethod CallNonvirtualBooleanMethodV CallNonvirtualBooleanMethodA, CallStaticBooleanMethod CallStaticBooleanMethodV CallStaticBooleanMethodA;
        'B': CallByteMethod CallByteMethodV CallByteMethodA, CallNonvirtualByteMethod CallNonvirtualByteMethodV CallNonvirtualByteMethodA, CallStaticByteMethod CallStaticByteMethodV CallStaticByteMethodA;
        'C': CallCharMethod CallCharMethodV CallCharMethodA, CallNonvirtualCharMethod CallNonvirtualCharMethodV CallNonvirtualCharMethodA, CallStaticCharMethod CallStaticCharMethodV CallStaticCharMethodA;
        'S': CallShortMethod CallShortMethodV CallShortMethodA, CallNonvirtualShortMethod CallNonvirtualShortMethodV CallNonvirtualShortMethodA, CallStaticShortMethod CallStaticShortMethodV CallStaticShortMethodA;
        'I': CallIntMethod CallIntMethodV CallIntMethodA, CallNonvirtualIntMethod CallNonvirtualIntMethodV CallNonvirtualIntMethodA, CallStaticIntMethod CallStaticIntMethodV CallStaticIntMethodA;
        'J': CallLongMethod CallLongMethodV CallLongMethodA, CallNonvirtualLongMethod CallNonvirtualLongMethodV CallNonvirtualLongMethodA, CallStaticLongMethod CallStaticLongMethodV CallStaticLongMethodA;
        'F': CallFloatMethod CallFloatMethodV CallFloatMethodA, CallNonvirtualFloatMethod CallNonvirtualFloatMethodV CallNonvirtualFloatMethodA, CallStaticFloatMethod CallStaticFloatMethodV CallStaticFloatMethodA;
        'D': CallDoubleMethod CallDoubleMethodV CallDoubleMethodA, CallNonvirtualDoubleMethod CallNonvirtualDoubleMethodV CallNonvirtualDoubleMethodA, CallStaticDoubleMethod CallStaticDoubleMethodV CallStaticDoubleMethodA;
        'V': CallVoidMethod CallVoidMethodV CallVoidMethodA, CallNonvirtualVoidMethod CallNonvirtualVoidMethodV CallNonvirtualVoidMethodA, CallStaticVoidMethod CallStaticVoidMethodV CallStaticVoidMethodA;
    }
    call_slot(slot!(NewObject), "JNI:NewObject", 'L', CallKind::New, VaKind::Dots);
    call_slot(slot!(NewObjectV), "JNI:NewObjectV", 'L', CallKind::New, VaKind::VaList);
    call_slot(slot!(NewObjectA), "JNI:NewObjectA", 'L', CallKind::New, VaKind::Array);

    hand!(GetMethodID, |c, env, a| {
        let t = a.next32();
        let cls = jni_to_host(c, t);
        let (name, sig): (*const c_char, *const c_char) = (a.ptr(), a.ptr());
        let id = jni!(env, GetMethodID, cls, name, sig);
        c.set_ret32(id_token(id as *mut c_void, &jni_shorty_from_signature(&cstr(sig))));
    });
    hand!(GetStaticMethodID, |c, env, a| {
        let t = a.next32();
        let cls = jni_to_host(c, t);
        let (name, sig): (*const c_char, *const c_char) = (a.ptr(), a.ptr());
        let id = jni!(env, GetStaticMethodID, cls, name, sig);
        c.set_ret32(id_token(id as *mut c_void, &jni_shorty_from_signature(&cstr(sig))));
    });
    hand!(FromReflectedMethod, |c, env, a| {
        let t = a.next32();
        let member = jni_to_host(c, t);
        let shorty = reflected_shorty(env, member);
        if shorty.is_empty() {
            logw!("FromReflectedMethod: could not read the method's parameter types");
        }
        let id = jni!(env, FromReflectedMethod, member);
        c.set_ret32(id_token(id as *mut c_void, &shorty));
    });

    // -- references --
    hand!(NewGlobalRef, |c, env, a| {
        let t = a.next32();
        let o = jni_to_host(c, t);
        c.set_ret32(new_global_token(jni!(env, NewGlobalRef, o)));
    });
    hand!(NewWeakGlobalRef, |c, env, a| {
        let t = a.next32();
        let o = jni_to_host(c, t);
        c.set_ret32(new_global_token(jni!(env, NewWeakGlobalRef, o)));
    });
    hand!(DeleteGlobalRef, |c, env, a| {
        let o = take_global_token(a.next32());
        if !o.is_null() {
            jni!(env, DeleteGlobalRef, o);
        }
    });
    hand!(DeleteWeakGlobalRef, |c, env, a| {
        let o = take_global_token(a.next32());
        if !o.is_null() {
            jni!(env, DeleteWeakGlobalRef, o);
        }
    });
    hand!(DeleteLocalRef, |c, env, a| {
        let tok = a.next32();
        let i = if (tok & TAG_MASK) == LOCAL_TAG { local_index(c, tok & !TAG_MASK) } else { None };
        if let Some(i) = i {
            if !c.locals[i].is_null() {
                jni!(env, DeleteLocalRef, c.locals[i] as jobject);
                if i + 1 == c.locals.len() {
                    c.locals.pop();
                    c.local_serials.pop();
                } else {
                    c.locals[i] = std::ptr::null_mut();
                }
            }
        }
    });

    // -- strings --
    hand!(GetStringUTFChars, |c, env, a| {
        let t = a.next32();
        let s = jni_to_host(c, t);
        let is_copy = a.next32();
        if s.is_null() {
            c.set_ret32(0);
            return;
        }
        let h = jni!(env, GetStringUTFChars, s, std::ptr::null_mut());
        let g = if h.is_null() { 0 } else { g_strdup(h) };
        if !h.is_null() {
            jni!(env, ReleaseStringUTFChars, s, h);
        }
        if is_copy != 0 {
            wr::<u8>(is_copy, JNI_TRUE);
        }
        c.set_ret32(g);
    });
    hand!(ReleaseStringUTFChars, |c, env, a| {
        a.next32();
        g_free(a.next32());
    });
    for (index, name) in [(slot!(GetStringChars), "JNI:GetStringChars"), (slot!(GetStringCritical), "JNI:GetStringCritical")] {
        set_slot(
            index,
            name,
            Box::new(|c: &mut Cpu| unsafe {
                let env = jni_host_env(c);
                let mut a = ArgReader::new(c);
                a.next32();
                let t = a.next32();
                let s = jni_to_host(c, t);
                let is_copy = a.next32();
                if s.is_null() {
                    c.set_ret32(0);
                    return;
                }
                let len = jni!(env, GetStringLength, s);
                let g = g_calloc(len as u32 + 1, 2);
                if g != 0 {
                    jni!(env, GetStringRegion, s, 0, len, g2h::<jchar>(g));
                }
                if is_copy != 0 {
                    wr::<u8>(is_copy, JNI_TRUE);
                }
                c.set_ret32(g);
            }),
        );
    }
    for (index, name) in [(slot!(ReleaseStringChars), "JNI:ReleaseStringChars"), (slot!(ReleaseStringCritical), "JNI:ReleaseStringCritical")] {
        set_slot(
            index,
            name,
            Box::new(|c: &mut Cpu| {
                let mut a = ArgReader::new(c);
                a.next32();
                a.next32();
                g_free(a.next32());
            }),
        );
    }

    // -- primitive array element access --
    let elems: [(usize, usize, &'static str, &'static str, i32); 9] = [
        (slot!(GetBooleanArrayElements), slot!(ReleaseBooleanArrayElements), "JNI:GetBooleanArrayElements", "JNI:ReleaseBooleanArrayElements", 0),
        (slot!(GetByteArrayElements), slot!(ReleaseByteArrayElements), "JNI:GetByteArrayElements", "JNI:ReleaseByteArrayElements", 1),
        (slot!(GetCharArrayElements), slot!(ReleaseCharArrayElements), "JNI:GetCharArrayElements", "JNI:ReleaseCharArrayElements", 2),
        (slot!(GetShortArrayElements), slot!(ReleaseShortArrayElements), "JNI:GetShortArrayElements", "JNI:ReleaseShortArrayElements", 3),
        (slot!(GetIntArrayElements), slot!(ReleaseIntArrayElements), "JNI:GetIntArrayElements", "JNI:ReleaseIntArrayElements", 4),
        (slot!(GetLongArrayElements), slot!(ReleaseLongArrayElements), "JNI:GetLongArrayElements", "JNI:ReleaseLongArrayElements", 5),
        (slot!(GetFloatArrayElements), slot!(ReleaseFloatArrayElements), "JNI:GetFloatArrayElements", "JNI:ReleaseFloatArrayElements", 6),
        (slot!(GetDoubleArrayElements), slot!(ReleaseDoubleArrayElements), "JNI:GetDoubleArrayElements", "JNI:ReleaseDoubleArrayElements", 7),
        (slot!(GetPrimitiveArrayCritical), slot!(ReleasePrimitiveArrayCritical), "JNI:GetPrimitiveArrayCritical", "JNI:ReleasePrimitiveArrayCritical", -1),
    ];
    for (get, release, get_name, release_name, kind) in elems {
        set_slot(
            get,
            get_name,
            Box::new(move |c: &mut Cpu| unsafe {
                let env = jni_host_env(c);
                let mut a = ArgReader::new(c);
                a.next32();
                let t = a.next32();
                let arr = jni_to_host(c, t);
                c.set_ret32(get_elements(env, arr, kind, a.next32()));
            }),
        );
        set_slot(
            release,
            release_name,
            Box::new(move |c: &mut Cpu| unsafe {
                let env = jni_host_env(c);
                let mut a = ArgReader::new(c);
                a.next32();
                let t = a.next32();
                let arr = jni_to_host(c, t);
                let buf = a.next32();
                release_elements(env, arr, kind, buf, a.i32());
            }),
        );
    }

    hand!(GetDirectBufferAddress, |c, env, a| {
        let t = a.next32();
        let buf = jni_to_host(c, t);
        let p = jni!(env, GetDirectBufferAddress, buf);
        if !p.is_null() && !mem_contains(p as usize as u64) {
            c.set_ret32(mirror_direct_buffer(env, buf, p));
            return;
        }
        c.set_ret32(p as usize as u32);
    });
    hand!(GetJavaVM, |c, env, a| {
        let out = a.next32();
        if out != 0 {
            wr::<u32>(out, G_GUEST_VM.load(Ordering::Relaxed));
        }
        c.set_ret32(0);
    });
    hand!(RegisterNatives, |c, env, a| {
        let t = a.next32();
        let cls = jni_to_host(c, t);
        let gm = a.next32();
        let n = a.i32();
        let mut hm: Vec<JNINativeMethod> = Vec::new();
        let mut ok = true;
        for i in 0..n.max(0) as u32 {
            let e: [u32; 3] = rd(gm + i * 12);
            let (name, signature) = (g2h::<c_char>(e[0]), g2h::<c_char>(e[1]));
            let fn_ptr = jni_make_trampoline(e[2], &jni_shorty_from_signature(&cstr(signature)), &cstr(name));
            ok &= !fn_ptr.is_null();
            hm.push(JNINativeMethod { name, signature, fnPtr: fn_ptr });
        }
        c.set_ret32(if ok { jni!(env, RegisterNatives, cls, hm.as_ptr(), n) } else { JNI_ERR } as u32);
    });

    // -- JavaVM --
    let invoke = g_calloc(8, 4);
    wr::<u32>(invoke + 12, raw_trap("JavaVM:DestroyJavaVM", |c| c.set_ret32(JNI_ERR as u32)));
    wr::<u32>(invoke + 16, raw_trap("JavaVM:AttachCurrentThread", |c| unsafe { vm_attach(c, false) }));
    wr::<u32>(invoke + 20, raw_trap("JavaVM:DetachCurrentThread", |c| unsafe { vm_detach(c) }));
    wr::<u32>(invoke + 24, raw_trap("JavaVM:GetEnv", |c| unsafe { vm_get_env(c) }));
    wr::<u32>(invoke + 28, raw_trap("JavaVM:AttachCurrentThreadAsDaemon", |c| unsafe { vm_attach(c, true) }));
    let guest_vm = g_calloc(1, 4);
    wr::<u32>(guest_vm, invoke);
    G_GUEST_VM.store(guest_vm, Ordering::SeqCst);
}

pub fn jni_bridge_init(vm: *mut JavaVM) -> bool {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        thunks_init();
        G_VM.store(vm, Ordering::SeqCst);
        build_tables();
        let (rc, env) = vm_env(JNI_VERSION_1_6);
        if rc == JNI_OK && !env.is_null() {
            for (i, kind) in ARRAY_KINDS.iter().enumerate() {
                let cls = jni!(env, FindClass, kind.0.as_ptr());
                ARRAY_CLASSES[i].store(jni!(env, NewGlobalRef, cls) as usize, Ordering::SeqCst);
                jni!(env, DeleteLocalRef, cls);
            }
        }
    });
    !jni_host_vm().is_null()
}

/// Host entry point for a guest native method (see trampoline.rs).
pub fn jni_make_trampoline(guest_fn: u32, shorty: &str, name: &str) -> *mut c_void {
    let mut g = lock(&METHOD_CACHE);
    let cache = g.get_or_insert_with(HashMap::new);
    let key = format!("{:08x}:{}", guest_fn, shorty);
    if let Some(&p) = cache.get(&key) {
        return p as *mut c_void;
    }
    let idx = METHOD_COUNT.load(Ordering::SeqCst);
    if idx >= MAX_TRAMPOLINES {
        loge!("too many native methods (limit {})", MAX_TRAMPOLINES);
        return std::ptr::null_mut();
    }
    let nm = Box::new(NativeMethod { guest_fn, shorty: shorty.to_string(), name: name.to_string(), calls: AtomicU64::new(0), reported: AtomicU64::new(0) });
    METHODS[idx as usize].store(Box::into_raw(nm), Ordering::Release);
    METHOD_COUNT.store(idx + 1, Ordering::SeqCst);
    let p = tramp_address(idx);
    cache.insert(key, p as usize);
    p
}

/// Called by the assembly stubs: marshals a host native-method call into the guest.
#[no_mangle]
pub unsafe extern "C" fn a64b_tramp_dispatch(index: u32, saved: *mut u64, stack: *mut u64) -> u64 {
    let m = if index < MAX_TRAMPOLINES { METHODS[index as usize].load(Ordering::Acquire).as_ref() } else { None };
    let Some(m) = m else { fatal!("native method trampoline {} is not bound", index) };
    m.calls.fetch_add(1, Ordering::Relaxed);
    let (mut ngpr, mut nfpr, mut nstack) = (0usize, 0usize, 0usize);
    let fprs = saved.add(GPR_COUNT);
    macro_rules! next_int {
        () => {{
            if ngpr < GPR_COUNT {
                ngpr += 1;
                *saved.add(ngpr - 1)
            } else {
                nstack += 1;
                *stack.add(nstack - 1)
            }
        }};
    }
    macro_rules! next_fp {
        () => {{
            if nfpr < 8 {
                nfpr += 1;
                *fprs.add(nfpr - 1)
            } else {
                nstack += 1;
                *stack.add(nstack - 1)
            }
        }};
    }

    let env = next_int!() as usize as *mut JNIEnv;
    let this = next_int!() as usize as jobject;
    let c = Cpu::current();
    let mut frame = JniFrame::new(c, env);

    let sh = m.shorty.as_bytes();
    let mut w: Vec<u32> = Vec::with_capacity(sh.len() * 2 + 2);
    w.push(guest_env_for(c));
    w.push(jni_new_local(c, this));
    frame.keep = 1;
    frame.keep_key = index + 1;
    for &t in &sh[1..] {
        match t {
            b'J' | b'D' => {
                let v = if t == b'J' { next_int!() } else { next_fp!() };
                if w.len() & 1 != 0 {
                    w.push(0); // 64-bit values are 8-byte aligned
                }
                w.push(v as u32);
                w.push((v >> 32) as u32);
            }
            b'F' => w.push(next_fp!() as u32),
            b'L' => {
                let o = next_int!() as usize as jobject;
                w.push(jni_new_local(c, o));
                frame.keep = c.locals.len() - frame.base;
            }
            // The 64-bit caller leaves the upper bits of sub-word values undefined; the 32-bit
            // callee expects them zero/sign extended.
            b'Z' => w.push((next_int!() & 0xff != 0) as u32),
            b'B' => w.push(next_int!() as i8 as i32 as u32),
            b'C' => w.push(next_int!() as u16 as u32),
            b'S' => w.push(next_int!() as i16 as i32 as u32),
            _ => w.push(next_int!() as u32),
        }
    }
    let mirrors = active_mirror_count();
    let r = c.call(m.guest_fn, &w);
    if active_mirror_count() > mirrors {
        // Back to Java: it sees what guest code wrote into mirrored direct buffers.
        flush_mirrors(mirrors);
        ACTIVE_MIRRORS.with(|m| m.borrow_mut().truncate(mirrors));
    }
    let result = match sh[0] {
        b'V' => 0,
        b'L' => jni_to_host(c, r as u32) as usize as u64,
        b'Z' => (r & 0xff != 0) as u64,
        b'B' => r as i8 as i64 as u64,
        b'C' => r & 0xffff,
        b'S' => r as i16 as i64 as u64,
        b'J' | b'D' => r,
        _ => r as u32 as u64, // I, F
    };
    drop(frame);
    result
}

// ---------------------------------------------------------------------------
// Binding of exported Java_* functions.
// ---------------------------------------------------------------------------
fn jni_mangle(s: &str) -> String {
    let mut out = String::new();
    for ch in s.bytes() {
        match ch {
            b'/' | b'.' => out.push('_'),
            b'_' => out += "_1",
            b';' => out += "_2",
            b'[' => out += "_3",
            c if c.is_ascii_alphanumeric() => out.push(c as char),
            c => out += &format!("_0{:04x}", c),
        }
    }
    out
}

// Splits "Java_com_foo_Bar_method[__args]" into class ("com/foo/Bar") and method name.
fn decode_symbol(sym: &str) -> Option<(String, String)> {
    let body = sym.as_bytes().get(5..)?;
    let body = match body.windows(2).position(|w| w == b"__") {
        Some(dbl) => &body[..dbl],
        None => body,
    };
    let mut path: Vec<u8> = Vec::new();
    let mut i = 0;
    while i < body.len() {
        if body[i] != b'_' {
            path.push(body[i]);
        } else if i + 1 < body.len() && body[i + 1] == b'1' {
            path.push(b'_');
            i += 1;
        } else if i + 5 < body.len() && body[i + 1] == b'0' {
            let hex = std::str::from_utf8(&body[i + 2..i + 6]).ok().and_then(|h| u32::from_str_radix(h, 16).ok()).unwrap_or(0);
            path.push(hex as u8);
            i += 5;
        } else {
            path.push(b'/');
        }
        i += 1;
    }
    let slash = path.iter().rposition(|&b| b == b'/')?;
    if slash == 0 || slash + 1 >= path.len() {
        return None;
    }
    Some((String::from_utf8_lossy(&path[..slash]).into_owned(), String::from_utf8_lossy(&path[slash + 1..]).into_owned()))
}

// JVM type descriptor of a java.lang.Class ("I", "Ljava/lang/String;", "[B").
unsafe fn descriptor_of(env: *mut JNIEnv, ty: jclass, get_name: jmethodID) -> String {
    let n = class_name(env, ty, get_name);
    if let Some(p) = PRIMS.iter().find(|p| p.0 == n) {
        return p.1.to_string();
    }
    let n = n.replace('.', "/");
    if n.starts_with('[') {
        n
    } else {
        format!("L{};", n)
    }
}

static APP_LOADER: AtomicUsize = AtomicUsize::new(0);

// Finds a class without initialising it. FindClass would run the static initialiser of every
// class that has a Java_* export, at a moment the app does not expect (during System.loadLibrary).
unsafe fn find_class_noinit(env: *mut JNIEnv, slash_name: &str) -> jclass {
    let loader = APP_LOADER.load(Ordering::Relaxed) as jobject;
    if !loader.is_null() {
        let dotted = cstring(&slash_name.replace('/', "."));
        let class_cls = jni!(env, FindClass, c"java/lang/Class".as_ptr());
        let for_name = jni!(env, GetStaticMethodID, class_cls, c"forName".as_ptr(), c"(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;".as_ptr());
        let jn = jni!(env, NewStringUTF, dotted.as_ptr());
        let args = [jvalue { l: jn }, jvalue { z: JNI_FALSE }, jvalue { l: loader }];
        let cls = jni!(env, CallStaticObjectMethodA, class_cls, for_name, args.as_ptr());
        jni!(env, DeleteLocalRef, jn);
        jni!(env, DeleteLocalRef, class_cls);
        if !clear_exception(env) && !cls.is_null() {
            return cls;
        }
    }
    let name = cstring(slash_name);
    let cls = jni!(env, FindClass, name.as_ptr());
    if clear_exception(env) {
        return std::ptr::null_mut();
    }
    cls
}

/// Class loader used to look up the app's classes without running their static initialisers.
pub unsafe fn jni_set_app_class_loader(env: *mut JNIEnv, loader: jobject) {
    if loader.is_null() || APP_LOADER.load(Ordering::SeqCst) != 0 {
        return;
    }
    APP_LOADER.store(jni!(env, NewGlobalRef, loader) as usize, Ordering::SeqCst);
}

/// Registers every exported Java_* function of the module with its Java class.
pub unsafe fn jni_bind_java_exports(env: *mut JNIEnv, m: ModRef) -> i32 {
    let mut by_class: HashMap<String, usize> = HashMap::new(); // class -> number of exported methods
    for sym in m.exports.keys() {
        if !sym.starts_with("Java_") {
            continue;
        }
        if let Some((cls, _method)) = decode_symbol(sym) {
            *by_class.entry(cls).or_insert(0) += 1;
        }
    }
    if by_class.is_empty() {
        return 0;
    }

    let class_cls = jni!(env, FindClass, c"java/lang/Class".as_ptr());
    let method_cls = jni!(env, FindClass, c"java/lang/reflect/Method".as_ptr());
    let get_methods = jni!(env, GetMethodID, class_cls, c"getDeclaredMethods".as_ptr(), c"()[Ljava/lang/reflect/Method;".as_ptr());
    let cls_name = jni!(env, GetMethodID, class_cls, c"getName".as_ptr(), c"()Ljava/lang/String;".as_ptr());
    let m_name = jni!(env, GetMethodID, method_cls, c"getName".as_ptr(), c"()Ljava/lang/String;".as_ptr());
    let m_mods = jni!(env, GetMethodID, method_cls, c"getModifiers".as_ptr(), c"()I".as_ptr());
    let m_params = jni!(env, GetMethodID, method_cls, c"getParameterTypes".as_ptr(), c"()[Ljava/lang/Class;".as_ptr());
    let m_ret = jni!(env, GetMethodID, method_cls, c"getReturnType".as_ptr(), c"()Ljava/lang/Class;".as_ptr());
    let none: *const jvalue = std::ptr::null();
    let mut bound = 0;

    for (class, exported) in &by_class {
        let cls = find_class_noinit(env, class);
        if cls.is_null() {
            logw!("class {} not found; its {} native method(s) stay unbound", class, exported);
            continue;
        }
        let methods = jni!(env, CallObjectMethodA, cls, get_methods, none);
        if clear_exception(env) || methods.is_null() {
            jni!(env, DeleteLocalRef, cls);
            continue;
        }
        let count = jni!(env, GetArrayLength, methods);
        let mangled_class = jni_mangle(class);
        for i in 0..count {
            let method = jni!(env, GetObjectArrayElement, methods, i);
            if jni!(env, CallIntMethodA, method, m_mods, none) & 0x100 == 0 {
                // not Modifier.NATIVE
                jni!(env, DeleteLocalRef, method);
                continue;
            }
            let jname = jni!(env, CallObjectMethodA, method, m_name, none);
            let name = jstring_text(env, jname).unwrap_or_default();
            jni!(env, DeleteLocalRef, jname);

            let mut args = String::new();
            let params = jni!(env, CallObjectMethodA, method, m_params, none);
            let np = jni!(env, GetArrayLength, params);
            for p in 0..np {
                let pt = jni!(env, GetObjectArrayElement, params, p);
                args += &descriptor_of(env, pt, cls_name);
                jni!(env, DeleteLocalRef, pt);
            }
            jni!(env, DeleteLocalRef, params);
            let rt = jni!(env, CallObjectMethodA, method, m_ret, none);
            let sig = format!("({}){}", args, descriptor_of(env, rt, cls_name));
            jni!(env, DeleteLocalRef, rt);
            jni!(env, DeleteLocalRef, method);

            let short_sym = format!("Java_{}_{}", mangled_class, jni_mangle(&name));
            let long_sym = format!("{}__{}", short_sym, jni_mangle(&args));
            let mut func = m.find(&long_sym);
            if func == 0 {
                func = m.find(&short_sym);
            }
            if func == 0 {
                logw!("no implementation for native method {}.{}{}", class, name, sig);
                continue;
            }
            let tramp = jni_make_trampoline(func, &jni_shorty_from_signature(&sig), &name);
            if tramp.is_null() {
                continue;
            }
            let (cname, csig) = (cstring(&name), cstring(&sig));
            let nm = JNINativeMethod { name: cname.as_ptr() as *mut c_char, signature: csig.as_ptr() as *mut c_char, fnPtr: tramp };
            if jni!(env, RegisterNatives, cls, &nm, 1) == JNI_OK {
                bound += 1;
            } else {
                clear_exception(env);
                logw!("could not register {}.{}{}", class, name, sig);
            }
        }
        jni!(env, DeleteLocalRef, methods);
        jni!(env, DeleteLocalRef, cls);
    }
    jni!(env, DeleteLocalRef, class_cls);
    jni!(env, DeleteLocalRef, method_cls);
    logi!("{}: bound {} Java_* native methods", m.name, bound);
    bound
}

/// Java -> guest native calls per method since the previous call of this function.
pub fn jni_call_stats() -> String {
    let mut v: Vec<(u64, &str)> = Vec::new();
    for m in methods() {
        let now = m.calls.load(Ordering::Relaxed);
        let before = m.reported.swap(now, Ordering::Relaxed);
        if now != before {
            v.push((now - before, &m.name));
        }
    }
    if v.is_empty() {
        return "none".into();
    }
    v.sort_by(|a, b| b.0.cmp(&a.0));
    v.iter().take(12).map(|(n, name)| format!("{} x{}", name, n)).collect::<Vec<_>>().join(", ")
}

/// Games that present their frames from Java (GLSurfaceView) never call eglSwapBuffers in native code.
/// Their frame rate is taken from the native method the renderer calls once per frame: the busiest
/// method whose name says so (0 if there is none).
pub fn jni_frame_method_calls() -> u64 {
    const WORDS: [&str; 6] = ["update", "render", "draw", "step", "frame", "tick"];
    methods()
        .filter(|m| {
            let name = m.name.to_ascii_lowercase();
            WORDS.iter().any(|w| name.contains(w))
        })
        .map(|m| m.calls.load(Ordering::Relaxed))
        .max()
        .unwrap_or(0)
}

/// Calls the guest JNI_OnLoad (if present). Returns the JNI version to report.
pub unsafe fn jni_call_onload(env: *mut JNIEnv, m: ModRef) -> jint {
    let func = m.find("JNI_OnLoad");
    if func == 0 {
        return JNI_VERSION_1_6;
    }
    let c = Cpu::current();
    let _frame = JniFrame::new(c, env);
    guest_env_for(c);
    let v = c.call(func, &[G_GUEST_VM.load(Ordering::Relaxed), 0]) as jint;
    logi!("{}: JNI_OnLoad returned 0x{:x}", m.name, v);
    v
}

/// Loads a guest library for Java code: load, bind natives, JNI_OnLoad.
pub unsafe fn jni_load_library(env: *mut JNIEnv, name: &str) -> Result<jint, String> {
    let mut vm: *mut JavaVM = std::ptr::null_mut();
    jni!(env, GetJavaVM, &mut vm);
    if !jni_bridge_init(vm) {
        return Err("bridge initialisation failed".into());
    }
    let c = Cpu::current();
    let fresh = loader_find(name).is_none();
    let m = {
        let _frame = JniFrame::new(c, env); // constructors may already use JNI through a cached JavaVM
        guest_env_for(c);
        loader_load(name)?
    };
    if !fresh {
        return Ok(JNI_VERSION_1_6);
    }
    jni_bind_java_exports(env, m);
    Ok(jni_call_onload(env, m))
}

/// Text of a Java string ("" for null).
pub unsafe fn jstr(env: *mut JNIEnv, s: jstring) -> String {
    jstring_text(env, s).unwrap_or_default()
}

/// A Java string for text (modified UTF-8 is assumed to be close enough for the messages used here).
pub unsafe fn new_jstring(env: *mut JNIEnv, s: &str) -> jstring {
    let c = CString::new(s.replace('\0', " ")).unwrap_or_default();
    jni!(env, NewStringUTF, c.as_ptr())
}
