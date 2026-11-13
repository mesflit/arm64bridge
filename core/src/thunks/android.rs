//! liblog, libandroid (assets, native window, configuration, loopers, sensors), EGL.
// SPDX-License-Identifier: GPL-2.0-or-later
#![allow(non_snake_case)]
use std::collections::HashMap;
use std::os::raw::{c_char, c_void};
use std::sync::Mutex;

use crate::cpu::Cpu;
use crate::guest_types::LogLimit;
use crate::limiter_probe::dump_guest_caller;
use crate::thunk::{reg, reg_raw, ArgReader, VaSource};
use crate::thunks::stdio::guest_format;
use crate::util::lock;

type Str = *const c_char;
type P = *mut c_void;

fn guest_log(prio: i32, tag: Str, msg: &[u8]) {
    let tag = unsafe { crate::util::cstr(tag) };
    crate::log::tagged(prio, if tag.is_empty() { "guest" } else { &tag }, &String::from_utf8_lossy(msg));
    // The first error messages of the guest come with the code that wrote them: a loader or check
    // inside a game often fails without saying why.
    static ERRORS: LogLimit = LogLimit::new();
    if prio >= 6 && ERRORS.below(2) {
        if let Some(c) = Cpu::current_or_null() {
            dump_guest_caller(c, "guest error message");
        }
    }
}

// 64-bit host pointers that the guest only stores and passes back (EGL handles,
// ANativeWindow*, AAsset*...) are replaced by small tokens.
struct Ptrs {
    list: Vec<usize>,
    lookup: HashMap<usize, u32>,
    asset_buffers: HashMap<u32, u32>, // AAsset token -> guest copy
}
static PTRS: Mutex<Option<Ptrs>> = Mutex::new(None);
const TOKEN_BASE: u32 = 0x00A6_4000;

fn ptrs<R>(f: impl FnOnce(&mut Ptrs) -> R) -> R {
    f(lock(&PTRS).get_or_insert_with(|| Ptrs { list: vec![0], lookup: HashMap::new(), asset_buffers: HashMap::new() }))
}

#[allow(dead_code)]
pub fn ptr_token<T>(p: *const T) -> u32 {
    if p.is_null() {
        return 0;
    }
    ptrs(|t| {
        if let Some(&tok) = t.lookup.get(&(p as usize)) {
            return tok;
        }
        let tok = TOKEN_BASE + t.list.len() as u32 * 4;
        t.list.push(p as usize);
        t.lookup.insert(p as usize, tok);
        tok
    })
}

#[allow(dead_code)]
pub fn ptr_host<T>(tok: u32) -> *mut T {
    if tok < TOKEN_BASE {
        return std::ptr::null_mut();
    }
    ptrs(|t| t.list.get(((tok - TOKEN_BASE) / 4) as usize).copied().unwrap_or(0)) as *mut T
}

#[allow(dead_code)]
fn ptr_drop(tok: u32) {
    if tok < TOKEN_BASE {
        return;
    }
    ptrs(|t| {
        let idx = ((tok - TOKEN_BASE) / 4) as usize;
        if idx < t.list.len() {
            let p = t.list[idx];
            t.lookup.remove(&p);
            t.list[idx] = 0;
        }
    })
}

pub fn register_android_thunks() {
    // ---------------------------------------------------------------- liblog
    reg("__android_log_write", |prio: i32, tag: Str, msg: Str| -> i32 {
        guest_log(prio, tag, unsafe { crate::util::cbytes(msg) });
        1
    });
    reg("__android_log_buf_write", |_buf: i32, prio: i32, tag: Str, msg: Str| -> i32 {
        guest_log(prio, tag, unsafe { crate::util::cbytes(msg) });
        1
    });
    // (name, has a leading buffer id, takes a va_list)
    for (name, buf, list) in [("__android_log_print", false, false), ("__android_log_vprint", false, true), ("__android_log_buf_print", true, false)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            if buf {
                a.next32();
            }
            let prio = a.i32();
            let tag: Str = a.ptr();
            let fmt: Str = a.ptr();
            let mut va = if list { VaSource::from_va_list(a.next32()) } else { VaSource::from_args(&a) };
            guest_log(prio, tag, &guest_format(fmt, &mut va));
            c.set_ret32(1);
        });
    }
    reg_raw("__android_log_assert", |c| unsafe {
        let mut a = ArgReader::new(c);
        let cond: Str = a.ptr();
        let tag: Str = a.ptr();
        let fmt: Str = a.ptr();
        let msg = if !fmt.is_null() {
            guest_format(fmt, &mut VaSource::from_args(&a))
        } else if !cond.is_null() {
            crate::util::cbytes(cond).to_vec()
        } else {
            b"assertion failed".to_vec()
        };
        guest_log(7, tag, &msg);
        fatal!("guest __android_log_assert: {}", String::from_utf8_lossy(&msg));
    });
    reg("__android_log_is_loggable", |_p: i32, _t: u32, _d: i32| -> i32 { 1 });

    crate::thunks::opensl::register_opensl_thunks();

    #[cfg(target_os = "android")]
    ndk::register();
}

#[cfg(target_os = "android")]
mod ndk {
    use super::*;
    use crate::cpu::{G_FRAMES, G_RENDER_TID};
    use crate::jni::{jni_host_env, jni_to_host};
    use crate::mem::*;
    use crate::thunk::thunk_resolve;
    use crate::thunks::libc::intern_cstr;
    use std::cell::Cell;
    use std::sync::atomic::Ordering;

    #[repr(C)]
    struct ASensorEvent([u8; 104]);

    #[link(name = "android")]
    extern "C" {
        fn ANativeWindow_fromSurface(env: P, surface: P) -> P;
        fn ANativeWindow_acquire(w: P);
        fn ANativeWindow_release(w: P);
        fn ANativeWindow_getWidth(w: P) -> i32;
        fn ANativeWindow_getHeight(w: P) -> i32;
        fn ANativeWindow_getFormat(w: P) -> i32;
        fn ANativeWindow_setBuffersGeometry(w: P, width: i32, height: i32, format: i32) -> i32;
        fn AAssetManager_fromJava(env: P, mgr: P) -> P;
        fn AAssetManager_open(mgr: P, name: Str, mode: i32) -> P;
        fn AAssetManager_openDir(mgr: P, path: Str) -> P;
        fn AAssetDir_getNextFileName(d: P) -> Str;
        fn AAssetDir_rewind(d: P);
        fn AAssetDir_close(d: P);
        fn AAsset_close(a: P);
        fn AAsset_read(a: P, buf: P, n: usize) -> i32;
        fn AAsset_seek(a: P, off: libc::off_t, whence: i32) -> libc::off_t;
        fn AAsset_seek64(a: P, off: i64, whence: i32) -> i64;
        fn AAsset_getLength(a: P) -> libc::off_t;
        fn AAsset_getLength64(a: P) -> i64;
        fn AAsset_getRemainingLength(a: P) -> libc::off_t;
        fn AAsset_getRemainingLength64(a: P) -> i64;
        fn AAsset_openFileDescriptor(a: P, start: *mut libc::off_t, length: *mut libc::off_t) -> i32;
        fn AAsset_getBuffer(a: P) -> *const c_void;
        fn AConfiguration_new() -> P;
        fn AConfiguration_delete(c: P);
        fn AConfiguration_fromAssetManager(c: P, mgr: P);
        fn AConfiguration_getLanguage(c: P, out: *mut c_char);
        fn AConfiguration_getCountry(c: P, out: *mut c_char);
        fn AConfiguration_getMcc(c: P) -> i32;
        fn AConfiguration_getMnc(c: P) -> i32;
        fn AConfiguration_getOrientation(c: P) -> i32;
        fn AConfiguration_getTouchscreen(c: P) -> i32;
        fn AConfiguration_getDensity(c: P) -> i32;
        fn AConfiguration_getKeyboard(c: P) -> i32;
        fn AConfiguration_getNavigation(c: P) -> i32;
        fn AConfiguration_getKeysHidden(c: P) -> i32;
        fn AConfiguration_getNavHidden(c: P) -> i32;
        fn AConfiguration_getSdkVersion(c: P) -> i32;
        fn AConfiguration_getScreenSize(c: P) -> i32;
        fn AConfiguration_getScreenLong(c: P) -> i32;
        fn AConfiguration_getUiModeType(c: P) -> i32;
        fn AConfiguration_getUiModeNight(c: P) -> i32;
        fn AConfiguration_getScreenWidthDp(c: P) -> i32;
        fn AConfiguration_getScreenHeightDp(c: P) -> i32;
        fn AConfiguration_getSmallestScreenWidthDp(c: P) -> i32;
        fn AConfiguration_getLayoutDirection(c: P) -> i32;
        fn ALooper_prepare(opts: i32) -> P;
        fn ALooper_forThread() -> P;
        fn ALooper_acquire(l: P);
        fn ALooper_release(l: P);
        fn ALooper_wake(l: P);
        fn ALooper_pollOnce(timeout: i32, fd: *mut i32, events: *mut i32, data: *mut P) -> i32;
        fn ALooper_addFd(l: P, fd: i32, ident: i32, events: i32, cb: P, data: P) -> i32;
        fn ALooper_removeFd(l: P, fd: i32) -> i32;
        fn ASensorManager_getInstance() -> P;
        fn ASensorManager_getInstanceForPackage(pkg: Str) -> P;
        fn ASensorManager_getDefaultSensor(m: P, ty: i32) -> *const c_void;
        fn ASensorManager_createEventQueue(m: P, looper: P, ident: i32, cb: P, data: P) -> P;
        fn ASensorManager_destroyEventQueue(m: P, q: P) -> i32;
        fn ASensorEventQueue_enableSensor(q: P, s: *const c_void) -> i32;
        fn ASensorEventQueue_disableSensor(q: P, s: *const c_void) -> i32;
        fn ASensorEventQueue_setEventRate(q: P, s: *const c_void, usec: i32) -> i32;
        fn ASensorEventQueue_hasEvents(q: P) -> i32;
        fn ASensorEventQueue_getEvents(q: P, ev: *mut ASensorEvent, n: usize) -> isize;
        fn ASensor_getName(s: *const c_void) -> Str;
        fn ASensor_getVendor(s: *const c_void) -> Str;
        fn ASensor_getType(s: *const c_void) -> i32;
        fn ASensor_getResolution(s: *const c_void) -> f32;
        fn ASensor_getMinDelay(s: *const c_void) -> i32;
    }

    #[link(name = "EGL")]
    extern "C" {
        fn eglGetError() -> i32;
        fn eglGetDisplay(native: P) -> P;
        fn eglInitialize(d: P, major: *mut i32, minor: *mut i32) -> u32;
        fn eglTerminate(d: P) -> u32;
        fn eglQueryString(d: P, name: i32) -> Str;
        fn eglBindAPI(api: u32) -> u32;
        fn eglSwapInterval(d: P, n: i32) -> u32;
        fn eglChooseConfig(d: P, attrs: *const i32, configs: *mut P, size: i32, num: *mut i32) -> u32;
        fn eglGetConfigs(d: P, configs: *mut P, size: i32, num: *mut i32) -> u32;
        fn eglGetConfigAttrib(d: P, cfg: P, attr: i32, value: *mut i32) -> u32;
        fn eglCreateWindowSurface(d: P, cfg: P, win: P, attrs: *const i32) -> P;
        fn eglCreatePbufferSurface(d: P, cfg: P, attrs: *const i32) -> P;
        fn eglDestroySurface(d: P, s: P) -> u32;
        fn eglQuerySurface(d: P, s: P, attr: i32, value: *mut i32) -> u32;
        fn eglSurfaceAttrib(d: P, s: P, attr: i32, value: i32) -> u32;
        fn eglCreateContext(d: P, cfg: P, share: P, attrs: *const i32) -> P;
        fn eglDestroyContext(d: P, ctx: P) -> u32;
        fn eglMakeCurrent(d: P, draw: P, read: P, ctx: P) -> u32;
        fn eglSwapBuffers(d: P, s: P) -> u32;
        fn eglGetCurrentContext() -> P;
        fn eglGetCurrentDisplay() -> P;
        fn eglGetCurrentSurface(which: i32) -> P;
        fn eglQueryContext(d: P, ctx: P, attr: i32, value: *mut i32) -> u32;
        fn eglWaitGL() -> u32;
        fn eglWaitNative(engine: i32) -> u32;
        fn eglReleaseThread() -> u32;
    }

    struct GAssetDir {
        dir: P,
        name: u32, // guest copy of the last returned file name
    }

    fn h(tok: u32) -> P {
        ptr_host::<c_void>(tok)
    }

    unsafe fn configs(d: u32, attrs: *const i32, out: *mut u32, size: i32, num: *mut i32, choose: bool) -> u32 {
        let mut tmp: Vec<P> = vec![std::ptr::null_mut(); size.max(0) as usize];
        let mut n = 0i32;
        let (buf, cap) = if out.is_null() { (std::ptr::null_mut(), 0) } else { (tmp.as_mut_ptr(), size) };
        let ok = if choose { eglChooseConfig(h(d), attrs, buf, cap, &mut n) } else { eglGetConfigs(h(d), buf, cap, &mut n) };
        if ok != 0 && !out.is_null() {
            for i in 0..n.min(size).max(0) as usize {
                *out.add(i) = ptr_token(tmp[i]);
            }
        }
        if !num.is_null() {
            *num = n;
        }
        ok
    }

    unsafe fn poll_once(timeout: i32, fd: *mut i32, events: *mut i32, data: *mut u32) -> i32 {
        let (mut f, mut e, mut d) = (0i32, 0i32, std::ptr::null_mut());
        let r = ALooper_pollOnce(timeout, &mut f, &mut e, &mut d);
        if !fd.is_null() {
            *fd = f;
        }
        if !events.is_null() {
            *events = e;
        }
        if !data.is_null() {
            *data = d as usize as u32;
        }
        r
    }

    macro_rules! r {
        ($name:expr, |$($a:ident : $t:ty),*| -> $ret:ty $body:block) => {
            reg($name, move |$($a: $t),*| -> $ret { unsafe { $body } })
        };
        ($name:expr, |$($a:ident : $t:ty),*| $body:block) => {
            reg($name, move |$($a: $t),*| -> () { unsafe { $body } })
        };
    }

    pub fn register() {
        // ---------------------------------------------------------------- native window
        reg_raw("ANativeWindow_fromSurface", |c| unsafe {
            let mut a = ArgReader::new(c);
            a.next32();
            let surface = jni_to_host(c, a.next32());
            let w = ANativeWindow_fromSurface(jni_host_env(c) as P, surface as P);
            c.set_ret32(ptr_token(w));
        });
        r!("ANativeWindow_acquire", |w: u32| {
            if !h(w).is_null() {
                ANativeWindow_acquire(h(w));
            }
        });
        r!("ANativeWindow_release", |w: u32| {
            if !h(w).is_null() {
                ANativeWindow_release(h(w));
            }
        });
        r!("ANativeWindow_getWidth", |w: u32| -> i32 { if h(w).is_null() { -1 } else { ANativeWindow_getWidth(h(w)) } });
        r!("ANativeWindow_getHeight", |w: u32| -> i32 { if h(w).is_null() { -1 } else { ANativeWindow_getHeight(h(w)) } });
        r!("ANativeWindow_getFormat", |w: u32| -> i32 { if h(w).is_null() { -1 } else { ANativeWindow_getFormat(h(w)) } });
        r!("ANativeWindow_setBuffersGeometry", |w: u32, width: i32, height: i32, format: i32| -> i32 {
            if h(w).is_null() { -1 } else { ANativeWindow_setBuffersGeometry(h(w), width, height, format) }
        });

        // ---------------------------------------------------------------- assets
        reg_raw("AAssetManager_fromJava", |c| unsafe {
            let mut a = ArgReader::new(c);
            a.next32();
            let mgr = jni_to_host(c, a.next32());
            let m = AAssetManager_fromJava(jni_host_env(c) as P, mgr as P);
            c.set_ret32(ptr_token(m));
        });
        r!("AAssetManager_open", |mgr: u32, name: Str, mode: i32| -> u32 { if h(mgr).is_null() { 0 } else { ptr_token(AAssetManager_open(h(mgr), name, mode)) } });
        r!("AAsset_close", |asset: u32| {
            let p = h(asset);
            if !p.is_null() {
                if let Some(copy) = ptrs(|t| t.asset_buffers.remove(&asset)) {
                    g_free(copy);
                }
                AAsset_close(p);
                ptr_drop(asset);
            }
        });
        r!("AAsset_read", |a: u32, buf: P, n: usize| -> i32 { if h(a).is_null() { -1 } else { AAsset_read(h(a), buf, n) } });
        r!("AAsset_seek", |a: u32, off: i32, wh: i32| -> i32 { if h(a).is_null() { -1 } else { AAsset_seek(h(a), off as libc::off_t, wh) as i32 } });
        r!("AAsset_seek64", |a: u32, off: i64, wh: i32| -> i64 { if h(a).is_null() { -1 } else { AAsset_seek64(h(a), off, wh) } });
        r!("AAsset_getLength", |a: u32| -> i32 { if h(a).is_null() { 0 } else { AAsset_getLength(h(a)) as i32 } });
        r!("AAsset_getLength64", |a: u32| -> i64 { if h(a).is_null() { 0 } else { AAsset_getLength64(h(a)) } });
        r!("AAsset_getRemainingLength", |a: u32| -> i32 { if h(a).is_null() { 0 } else { AAsset_getRemainingLength(h(a)) as i32 } });
        r!("AAsset_getRemainingLength64", |a: u32| -> i64 { if h(a).is_null() { 0 } else { AAsset_getRemainingLength64(h(a)) } });
        reg("AAsset_isAllocated", |_a: u32| -> i32 { 0 });
        r!("AAsset_openFileDescriptor", |a: u32, start: *mut i32, length: *mut i32| -> i32 {
            if h(a).is_null() {
                return -1;
            }
            let (mut s, mut l): (libc::off_t, libc::off_t) = (0, 0);
            let fd = AAsset_openFileDescriptor(h(a), &mut s, &mut l);
            if fd >= 0 {
                *start = s as i32;
                *length = l as i32;
            }
            fd
        });

        // ---------------------------------------------------------------- asset directories
        r!("AAssetManager_openDir", |mgr: u32, path: Str| -> u32 {
            if h(mgr).is_null() {
                return 0;
            }
            let d = AAssetManager_openDir(h(mgr), if path.is_null() { c"".as_ptr() } else { path });
            if d.is_null() {
                return 0;
            }
            ptr_token(Box::into_raw(Box::new(GAssetDir { dir: d, name: g_malloc(1024) })))
        });
        r!("AAssetDir_getNextFileName", |tok: u32| -> u32 {
            let g = ptr_host::<GAssetDir>(tok);
            if g.is_null() {
                return 0;
            }
            let n = AAssetDir_getNextFileName((*g).dir);
            if n.is_null() {
                return 0;
            }
            let b = crate::util::cbytes(n);
            let len = b.len().min(1023);
            std::ptr::copy_nonoverlapping(b.as_ptr(), g2h::<u8>((*g).name), len);
            wr::<u8>((*g).name + len as u32, 0);
            (*g).name
        });
        r!("AAssetDir_rewind", |tok: u32| {
            let g = ptr_host::<GAssetDir>(tok);
            if !g.is_null() {
                AAssetDir_rewind((*g).dir);
            }
        });
        r!("AAssetDir_close", |tok: u32| {
            let g = ptr_host::<GAssetDir>(tok);
            if g.is_null() {
                return;
            }
            AAssetDir_close((*g).dir);
            g_free((*g).name);
            ptr_drop(tok);
            drop(Box::from_raw(g));
        });
        r!("AAsset_getBuffer", |asset: u32| -> u32 {
            // The asset's own buffer is outside guest memory: hand out a guest copy, freed by AAsset_close.
            let p = h(asset);
            if p.is_null() {
                return 0;
            }
            if let Some(g) = ptrs(|t| t.asset_buffers.get(&asset).copied()) {
                return g;
            }
            let buf = AAsset_getBuffer(p);
            let len = AAsset_getLength64(p);
            if buf.is_null() || !(0..=0x7fff_ffff).contains(&len) {
                return 0;
            }
            let g = g_malloc(len as u32 + 1);
            if g == 0 {
                return 0;
            }
            std::ptr::copy_nonoverlapping(buf as *const u8, g2h::<u8>(g), len as usize);
            ptrs(|t| t.asset_buffers.insert(asset, g));
            g
        });

        // ---------------------------------------------------------------- configuration
        r!("AConfiguration_new", | | -> u32 { ptr_token(AConfiguration_new()) });
        r!("AConfiguration_delete", |c: u32| {
            if !h(c).is_null() {
                AConfiguration_delete(h(c));
                ptr_drop(c);
            }
        });
        r!("AConfiguration_fromAssetManager", |c: u32, mgr: u32| {
            if !h(c).is_null() && !h(mgr).is_null() {
                AConfiguration_fromAssetManager(h(c), h(mgr));
            }
        });
        r!("AConfiguration_getLanguage", |c: u32, out: *mut c_char| {
            if !h(c).is_null() {
                AConfiguration_getLanguage(h(c), out);
            }
        });
        r!("AConfiguration_getCountry", |c: u32, out: *mut c_char| {
            if !h(c).is_null() {
                AConfiguration_getCountry(h(c), out);
            }
        });
        macro_rules! acfg {
            ($($f:ident)*) => {$(
                r!(stringify!($f), |c: u32| -> i32 { if h(c).is_null() { 0 } else { $f(h(c)) } });
            )*};
        }
        acfg!(AConfiguration_getMcc AConfiguration_getMnc AConfiguration_getOrientation AConfiguration_getTouchscreen AConfiguration_getDensity
              AConfiguration_getKeyboard AConfiguration_getNavigation AConfiguration_getKeysHidden AConfiguration_getNavHidden
              AConfiguration_getSdkVersion AConfiguration_getScreenSize AConfiguration_getScreenLong AConfiguration_getUiModeType
              AConfiguration_getUiModeNight AConfiguration_getScreenWidthDp AConfiguration_getScreenHeightDp
              AConfiguration_getSmallestScreenWidthDp AConfiguration_getLayoutDirection);

        // ---------------------------------------------------------------- loopers and sensors
        r!("ALooper_prepare", |opts: i32| -> u32 { ptr_token(ALooper_prepare(opts)) });
        r!("ALooper_forThread", | | -> u32 { ptr_token(ALooper_forThread()) });
        r!("ALooper_acquire", |l: u32| {
            if !h(l).is_null() {
                ALooper_acquire(h(l));
            }
        });
        r!("ALooper_release", |l: u32| {
            if !h(l).is_null() {
                ALooper_release(h(l));
            }
        });
        r!("ALooper_wake", |l: u32| {
            if !h(l).is_null() {
                ALooper_wake(h(l));
            }
        });
        for n in ["ALooper_pollOnce", "ALooper_pollAll"] {
            r!(n, |timeout: i32, fd: *mut i32, events: *mut i32, data: *mut u32| -> i32 { poll_once(timeout, fd, events, data) });
        }
        r!("ALooper_addFd", |l: u32, fd: i32, ident: i32, events: i32, cb: u32, data: u32| -> i32 {
            if cb != 0 {
                logw!("ALooper_addFd with a callback is not bridged; the fd is only reported by ident");
            }
            if h(l).is_null() { -1 } else { ALooper_addFd(h(l), fd, ident, events, std::ptr::null_mut(), data as usize as P) }
        });
        r!("ALooper_removeFd", |l: u32, fd: i32| -> i32 { if h(l).is_null() { -1 } else { ALooper_removeFd(h(l), fd) } });
        r!("ASensorManager_getInstance", | | -> u32 { ptr_token(ASensorManager_getInstance()) });
        r!("ASensorManager_getInstanceForPackage", |pkg: Str| -> u32 { ptr_token(ASensorManager_getInstanceForPackage(pkg)) });
        r!("ASensorManager_getDefaultSensor", |m: u32, ty: i32| -> u32 { if h(m).is_null() { 0 } else { ptr_token(ASensorManager_getDefaultSensor(h(m), ty)) } });
        r!("ASensorManager_createEventQueue", |m: u32, looper: u32, ident: i32, cb: u32, data: u32| -> u32 {
            if h(m).is_null() {
                return 0;
            }
            if cb != 0 {
                logw!("ASensorManager_createEventQueue with a callback is not bridged; events must be read with ASensorEventQueue_getEvents");
            }
            ptr_token(ASensorManager_createEventQueue(h(m), h(looper), ident, std::ptr::null_mut(), data as usize as P))
        });
        r!("ASensorManager_destroyEventQueue", |m: u32, q: u32| -> i32 {
            if h(m).is_null() || h(q).is_null() {
                return -1;
            }
            let r = ASensorManager_destroyEventQueue(h(m), h(q));
            ptr_drop(q);
            r
        });
        r!("ASensorEventQueue_enableSensor", |q: u32, s: u32| -> i32 { if h(q).is_null() { -1 } else { ASensorEventQueue_enableSensor(h(q), h(s)) } });
        r!("ASensorEventQueue_disableSensor", |q: u32, s: u32| -> i32 { if h(q).is_null() { -1 } else { ASensorEventQueue_disableSensor(h(q), h(s)) } });
        r!("ASensorEventQueue_setEventRate", |q: u32, s: u32, usec: i32| -> i32 { if h(q).is_null() { -1 } else { ASensorEventQueue_setEventRate(h(q), h(s), usec) } });
        r!("ASensorEventQueue_hasEvents", |q: u32| -> i32 { if h(q).is_null() { -1 } else { ASensorEventQueue_hasEvents(h(q)) } });
        // ASensorEvent has the same 104-byte layout on 32- and 64-bit ARM.
        r!("ASensorEventQueue_getEvents", |q: u32, ev: *mut ASensorEvent, n: u32| -> i32 {
            if h(q).is_null() { -1 } else { ASensorEventQueue_getEvents(h(q), ev, n as usize) as i32 }
        });
        r!("ASensor_getName", |s: u32| -> u32 { if h(s).is_null() { 0 } else { intern_cstr(ASensor_getName(h(s))) } });
        r!("ASensor_getVendor", |s: u32| -> u32 { if h(s).is_null() { 0 } else { intern_cstr(ASensor_getVendor(h(s))) } });
        r!("ASensor_getType", |s: u32| -> i32 { if h(s).is_null() { -1 } else { ASensor_getType(h(s)) } });
        r!("ASensor_getResolution", |s: u32| -> f32 { if h(s).is_null() { 0.0 } else { ASensor_getResolution(h(s)) } });
        r!("ASensor_getMinDelay", |s: u32| -> i32 { if h(s).is_null() { 0 } else { ASensor_getMinDelay(h(s)) } });

        // ---------------------------------------------------------------- EGL
        r!("eglGetError", | | -> i32 { eglGetError() });
        r!("eglGetDisplay", |native: u32| -> u32 { ptr_token(eglGetDisplay(h(native))) });
        r!("eglInitialize", |d: u32, major: *mut i32, minor: *mut i32| -> u32 { eglInitialize(h(d), major, minor) });
        r!("eglTerminate", |d: u32| -> u32 { eglTerminate(h(d)) });
        r!("eglQueryString", |d: u32, name: i32| -> u32 { intern_cstr(eglQueryString(h(d), name)) });
        r!("eglBindAPI", |api: u32| -> u32 { eglBindAPI(api) });
        r!("eglSwapInterval", |d: u32, n: i32| -> u32 { eglSwapInterval(h(d), n) });
        r!("eglChooseConfig", |d: u32, attrs: *const i32, out: *mut u32, size: i32, num: *mut i32| -> u32 { configs(d, attrs, out, size, num, true) });
        r!("eglGetConfigs", |d: u32, out: *mut u32, size: i32, num: *mut i32| -> u32 { configs(d, std::ptr::null(), out, size, num, false) });
        r!("eglGetConfigAttrib", |d: u32, cfg: u32, attr: i32, value: *mut i32| -> u32 { eglGetConfigAttrib(h(d), h(cfg), attr, value) });
        r!("eglCreateWindowSurface", |d: u32, cfg: u32, win: u32, attrs: *const i32| -> u32 { ptr_token(eglCreateWindowSurface(h(d), h(cfg), h(win), attrs)) });
        r!("eglCreatePbufferSurface", |d: u32, cfg: u32, attrs: *const i32| -> u32 { ptr_token(eglCreatePbufferSurface(h(d), h(cfg), attrs)) });
        r!("eglDestroySurface", |d: u32, s: u32| -> u32 { eglDestroySurface(h(d), h(s)) });
        r!("eglQuerySurface", |d: u32, s: u32, attr: i32, value: *mut i32| -> u32 { eglQuerySurface(h(d), h(s), attr, value) });
        r!("eglSurfaceAttrib", |d: u32, s: u32, attr: i32, value: i32| -> u32 { eglSurfaceAttrib(h(d), h(s), attr, value) });
        r!("eglCreateContext", |d: u32, cfg: u32, share: u32, attrs: *const i32| -> u32 { ptr_token(eglCreateContext(h(d), h(cfg), h(share), attrs)) });
        r!("eglDestroyContext", |d: u32, ctx: u32| -> u32 { eglDestroyContext(h(d), h(ctx)) });
        r!("eglMakeCurrent", |d: u32, draw: u32, read: u32, ctx: u32| -> u32 { eglMakeCurrent(h(d), h(draw), h(read), h(ctx)) });
        r!("eglSwapBuffers", |d: u32, s: u32| -> u32 {
            G_FRAMES.fetch_add(1, Ordering::Relaxed);
            G_RENDER_TID.store(crate::util::gettid(), Ordering::Relaxed);
            crate::limiter_probe::probe_frame();
            thread_local! { static BOOSTED: Cell<bool> = const { Cell::new(false) }; }
            if !BOOSTED.with(|b| b.replace(true)) {
                // The thread that presents frames gets display priority, like Android's own RenderThread.
                let mut r = libc::setpriority(libc::PRIO_PROCESS as _, 0, -10);
                if r != 0 {
                    r = libc::setpriority(libc::PRIO_PROCESS as _, 0, -8);
                }
                logi!("render thread {}: priority {}", crate::util::gettid(), if r == 0 { "raised to display level" } else { "unchanged (not permitted)" });
            }
            eglSwapBuffers(h(d), h(s))
        });
        r!("eglGetCurrentContext", | | -> u32 { ptr_token(eglGetCurrentContext()) });
        r!("eglGetCurrentDisplay", | | -> u32 { ptr_token(eglGetCurrentDisplay()) });
        r!("eglGetCurrentSurface", |which: i32| -> u32 { ptr_token(eglGetCurrentSurface(which)) });
        r!("eglQueryContext", |d: u32, ctx: u32, attr: i32, value: *mut i32| -> u32 { eglQueryContext(h(d), h(ctx), attr, value) });
        r!("eglWaitGL", | | -> u32 { eglWaitGL() });
        r!("eglWaitNative", |engine: i32| -> u32 { eglWaitNative(engine) });
        r!("eglReleaseThread", | | -> u32 { eglReleaseThread() });
        r!("eglGetProcAddress", |name: Str| -> u32 { thunk_resolve(&crate::util::cstr(name)) });
    }
}
