//! Public entry points: the shim loader used inside patched apps and the JNI
//! methods of the ARM64Bridge app itself.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::collections::HashSet;
use std::ffi::c_void;
use std::os::raw::c_char;
use std::sync::atomic::Ordering;
use std::sync::{Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

use jni_sys::*;

use crate::cpu::{cpu_profile_report, cpu_total_host_calls, G_FRAMES, G_FRAME_LIMITER, G_GUEST_THREADS, G_RENDER_TID};
use crate::elf::{elf_inspect, loader_add_provider};
use crate::jni::{jni_call_stats, jni_frame_method_calls, jni_load_library, jni_set_app_class_loader, jstr, new_jstring};
use crate::thunk::thunk_exists;
use crate::util::{cstr, cstring, lock};
use crate::zip::zip_extract;
use crate::{jni, VERSION};

static SOURCES: Mutex<Option<HashSet<String>>> = Mutex::new(None);
static APK_PATH: OnceLock<String> = OnceLock::new();

fn add_source(key: String) -> bool {
    lock(&SOURCES).get_or_insert_with(HashSet::new).insert(key)
}

fn add_guest_dir(dir: &str) {
    if !add_source(format!("dir:{}", dir)) {
        return;
    }
    logi!("guest library directory: {}", dir);
    let dir = dir.to_string();
    loader_add_provider(Box::new(move |name| std::fs::read(format!("{}/{}", dir, name)).ok().filter(|v| !v.is_empty())));
}

fn add_guest_apk(apk: &str) {
    if !add_source(format!("apk:{}", apk)) {
        return;
    }
    let _ = APK_PATH.set(apk.to_string());
    logi!("guest libraries from APK: {}", apk);
    let apk = apk.to_string();
    loader_add_provider(Box::new(move |name| {
        zip_extract(&apk, &format!("lib/armeabi-v7a/{}", name)).or_else(|| zip_extract(&apk, &format!("lib/armeabi/{}", name)))
    }));
}

fn init_from_environment() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        if let Ok(v) = std::env::var("A64B_LOG") {
            if v.starts_with(['d', 'D']) {
                crate::log::LOG_LEVEL.store(crate::log::LOG_DEBUG, Ordering::Relaxed);
            }
        }
        if let Ok(d) = std::env::var("A64B_GUEST_DIR") {
            add_guest_dir(&d);
        }
    });
}

unsafe fn clear_exc(env: *mut JNIEnv) -> bool {
    if jni!(env, ExceptionCheck) == 0 {
        return false;
    }
    jni!(env, ExceptionClear);
    true
}

const NO_ARGS: *const jvalue = std::ptr::null();

unsafe fn method(env: *mut JNIEnv, obj: jobject, name: &std::ffi::CStr, sig: &std::ffi::CStr) -> jmethodID {
    let cls = jni!(env, GetObjectClass, obj);
    let m = jni!(env, GetMethodID, cls, name.as_ptr(), sig.as_ptr());
    jni!(env, DeleteLocalRef, cls);
    m
}

unsafe fn call_obj(env: *mut JNIEnv, obj: jobject, name: &std::ffi::CStr, sig: &std::ffi::CStr, args: &[jvalue]) -> jobject {
    let m = method(env, obj, name, sig);
    if m.is_null() {
        clear_exc(env);
        return std::ptr::null_mut();
    }
    let r = jni!(env, CallObjectMethodA, obj, m, if args.is_empty() { NO_ARGS } else { args.as_ptr() });
    if clear_exc(env) {
        return std::ptr::null_mut();
    }
    r
}

unsafe fn current_application(env: *mut JNIEnv) -> jobject {
    let at = jni!(env, FindClass, c"android/app/ActivityThread".as_ptr());
    if clear_exc(env) || at.is_null() {
        return std::ptr::null_mut();
    }
    let cur = jni!(env, GetStaticMethodID, at, c"currentApplication".as_ptr(), c"()Landroid/app/Application;".as_ptr());
    let app = if cur.is_null() { std::ptr::null_mut() } else { jni!(env, CallStaticObjectMethodA, at, cur, NO_ARGS) };
    let app = if clear_exc(env) { std::ptr::null_mut() } else { app };
    jni!(env, DeleteLocalRef, at);
    app
}

unsafe fn static_int(env: *mut JNIEnv, cls_name: &std::ffi::CStr, field: &std::ffi::CStr) -> i32 {
    let cls = jni!(env, FindClass, cls_name.as_ptr());
    if clear_exc(env) || cls.is_null() {
        return 0;
    }
    let f = jni!(env, GetStaticFieldID, cls, field.as_ptr(), c"I".as_ptr());
    let v = if f.is_null() { 0 } else { jni!(env, GetStaticIntField, cls, f) };
    clear_exc(env);
    jni!(env, DeleteLocalRef, cls);
    v
}

unsafe fn static_string(env: *mut JNIEnv, cls_name: &std::ffi::CStr, field: &std::ffi::CStr) -> String {
    let cls = jni!(env, FindClass, cls_name.as_ptr());
    if clear_exc(env) || cls.is_null() {
        return String::new();
    }
    let f = jni!(env, GetStaticFieldID, cls, field.as_ptr(), c"Ljava/lang/String;".as_ptr());
    let v = if f.is_null() { std::ptr::null_mut() } else { jni!(env, GetStaticObjectField, cls, f) };
    clear_exc(env);
    let r = jstr(env, v);
    jni!(env, DeleteLocalRef, cls);
    r
}

// Creates Download/ARM64Bridge/<name> through MediaStore (Android 10+, no permission needed)
// and returns a raw file descriptor for it, or -1.
unsafe fn open_download_file(env: *mut JNIEnv, app: jobject, name: &str) -> i32 {
    if static_int(env, c"android/os/Build$VERSION", c"SDK_INT") < 29 {
        return -1;
    }
    let cr = call_obj(env, app, c"getContentResolver", c"()Landroid/content/ContentResolver;", &[]);
    let cv_cls = jni!(env, FindClass, c"android/content/ContentValues".as_ptr());
    let dl_cls = jni!(env, FindClass, c"android/provider/MediaStore$Downloads".as_ptr());
    if clear_exc(env) || cr.is_null() || cv_cls.is_null() || dl_cls.is_null() {
        return -1;
    }
    let ctor = jni!(env, GetMethodID, cv_cls, c"<init>".as_ptr(), c"()V".as_ptr());
    let cv = jni!(env, NewObjectA, cv_cls, ctor, NO_ARGS);
    let put = jni!(env, GetMethodID, cv_cls, c"put".as_ptr(), c"(Ljava/lang/String;Ljava/lang/String;)V".as_ptr());
    for (k, v) in [("_display_name", name), ("mime_type", "text/plain"), ("relative_path", "Download/ARM64Bridge")] {
        let (jk, jv) = (new_jstring(env, k), new_jstring(env, v));
        jni!(env, CallVoidMethodA, cv, put, [jvalue { l: jk }, jvalue { l: jv }].as_ptr());
        jni!(env, DeleteLocalRef, jk);
        jni!(env, DeleteLocalRef, jv);
    }
    let uri_f = jni!(env, GetStaticFieldID, dl_cls, c"EXTERNAL_CONTENT_URI".as_ptr(), c"Landroid/net/Uri;".as_ptr());
    let base = if uri_f.is_null() { std::ptr::null_mut() } else { jni!(env, GetStaticObjectField, dl_cls, uri_f) };
    let mut fd = -1;
    let uri = if base.is_null() {
        std::ptr::null_mut()
    } else {
        call_obj(env, cr, c"insert", c"(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;", &[jvalue { l: base }, jvalue { l: cv }])
    };
    if !uri.is_null() {
        let mode = new_jstring(env, "w");
        let pfd = call_obj(env, cr, c"openFileDescriptor", c"(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;", &[jvalue { l: uri }, jvalue { l: mode }]);
        if !pfd.is_null() {
            let detach = method(env, pfd, c"detachFd", c"()I");
            fd = jni!(env, CallIntMethodA, pfd, detach, NO_ARGS);
            if clear_exc(env) {
                fd = -1;
            }
        }
    }
    clear_exc(env);
    fd
}

/// /proc/self/task/<tid>/stat fields after the ")" that ends the thread name.
fn thread_stat_fields(tid: i32) -> Option<Vec<String>> {
    let s = std::fs::read_to_string(format!("/proc/self/task/{}/stat", tid)).ok()?;
    let p = s.rfind(')')?;
    Some(s[p + 1..].split_whitespace().map(|x| x.to_string()).collect())
}

// CPU core a thread last ran on (field 39 of the stat file), or -1.
fn thread_cpu_core(tid: i32) -> i32 {
    thread_stat_fields(tid).and_then(|f| f.get(39 - 3).and_then(|v| v.parse().ok())).unwrap_or(-1)
}

// CPU time (user + system, in clock ticks) a thread has used so far: fields 14 and 15.
fn thread_cpu_ticks(tid: i32) -> i64 {
    thread_stat_fields(tid)
        .and_then(|f| Some(f.get(14 - 3)?.parse::<i64>().ok()? + f.get(15 - 3)?.parse::<i64>().ok()?))
        .unwrap_or(-1)
}

fn process_cpu_ticks() -> i64 {
    std::fs::read_to_string("/proc/self/stat")
        .ok()
        .and_then(|s| {
            let f: Vec<&str> = s[s.rfind(')')? + 1..].split_whitespace().collect();
            Some(f.get(11)?.parse::<i64>().ok()? + f.get(12)?.parse::<i64>().ok()?)
        })
        .unwrap_or(0)
}

// Every 5 s: frames drawn, guest->host calls and Java->native calls. Shows whether a
// "frozen" game still renders and whether touch events reach the native code.
fn heartbeat() {
    let (mut last_frames, mut last_calls, mut last_tid, mut last_ticks, mut last_proc) = (0u64, 0u64, 0i32, 0i64, 0i64);
    let hz = unsafe { libc::sysconf(libc::_SC_CLK_TCK) }.max(1) as i64;
    loop {
        std::thread::sleep(Duration::from_secs(5));
        let mut f = G_FRAMES.load(Ordering::Relaxed);
        if f == 0 {
            f = jni_frame_method_calls(); // frames presented from Java
        }
        let k = cpu_total_host_calls();
        // How busy the render thread and the whole process are: a render thread at ~100%
        // means the frame rate is limited by how fast guest code is emulated.
        let tid = G_RENDER_TID.load(Ordering::Relaxed);
        let ticks = if tid != 0 { thread_cpu_ticks(tid) } else { -1 };
        let proc = process_cpu_ticks();
        let render_pct = if tid != 0 && tid == last_tid && ticks >= 0 { (ticks - last_ticks) * 100 / (5 * hz) } else { -1 };
        let proc_pct = if last_proc != 0 { (proc - last_proc) * 100 / (5 * hz) } else { -1 };
        logi!(
            "heartbeat: {:.1} fps, render thread CPU {}% (core {}), process CPU {}%, {} guest->host calls/s, {} guest threads; Java->native: {}",
            f.wrapping_sub(last_frames) as f64 / 5.0,
            render_pct,
            if tid != 0 { thread_cpu_core(tid) } else { -1 },
            proc_pct,
            if k >= last_calls { (k - last_calls) / 5 } else { 0 }, // the total drops when threads end
            G_GUEST_THREADS.load(Ordering::Relaxed),
            jni_call_stats()
        );
        if tid != 0 {
            let prof = cpu_profile_report(tid, 5.0);
            if !prof.is_empty() {
                logi!("profile: {}", prof);
            }
        }
        (last_tid, last_ticks, last_proc, last_frames, last_calls) = (tid, ticks, proc, f, k);
    }
}

// Every patched app writes its bridge log to Download/ARM64Bridge/ (or, before Android 10,
// to Android/data/<package>/files/) so that problems can be reported without a computer.
unsafe fn open_log_file(env: *mut JNIEnv) {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let app = current_application(env);
        if app.is_null() {
            return;
        }
        let pkg = jstr(env, call_obj(env, app, c"getPackageName", c"()Ljava/lang/String;", &[]));
        let loader = call_obj(env, app, c"getClassLoader", c"()Ljava/lang/ClassLoader;", &[]);
        jni_set_app_class_loader(env, loader);

        let mut now: libc::time_t = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&mut now, &mut tm);
        let stamp = format!("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        let name = format!("arm64bridge-{}-{}.log", pkg, stamp);
        let mut fd = open_download_file(env, app, &name);
        let mut place = format!("Download/ARM64Bridge/{}", name);
        if fd < 0 {
            let dir = call_obj(env, app, c"getExternalFilesDir", c"(Ljava/lang/String;)Ljava/io/File;", &[jvalue { l: std::ptr::null_mut() }]);
            if !dir.is_null() {
                let path = jstr(env, call_obj(env, dir, c"getAbsolutePath", c"()Ljava/lang/String;", &[]));
                place = format!("{}/{}", path, name);
                let p = cstring(&place);
                fd = libc::open(p.as_ptr(), libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC | libc::O_CLOEXEC, 0o644);
            }
        }
        if fd >= 0 {
            crate::log::set_file(fd);
        }
        std::thread::spawn(heartbeat);
        logi!("ARM64Bridge {}, log file: {}", VERSION, if fd >= 0 { place.as_str() } else { "(none)" });
        logi!(
            "app {} on {} {}, Android {} (API {})",
            pkg,
            static_string(env, c"android/os/Build", c"MANUFACTURER"),
            static_string(env, c"android/os/Build", c"MODEL"),
            static_string(env, c"android/os/Build$VERSION", c"RELEASE"),
            static_int(env, c"android/os/Build$VERSION", c"SDK_INT")
        );
    });
}

// ---- in-game menu (com.mesflit.arm64bridge.overlay.GameOverlay, loaded from the patched APK) ----
unsafe extern "system" fn overlay_set_limiter(_env: *mut JNIEnv, _cls: jclass, mode: jint) {
    G_FRAME_LIMITER.store(if (0..=2).contains(&mode) { mode } else { 0 }, Ordering::Relaxed);
    logi!("in-game menu: frame limiter mode {}", mode);
}

unsafe extern "system" fn overlay_version(env: *mut JNIEnv, _cls: jclass) -> jstring {
    new_jstring(env, VERSION)
}

// A Java exception that is about to end the app (uncaught), written to the bridge log.
unsafe extern "system" fn overlay_log_crash(env: *mut JNIEnv, _cls: jclass, text: jstring) {
    let t = jstr(env, text);
    loge!("=== uncaught Java exception (the app is closing) ===");
    for line in t.lines().take(80) {
        loge!("  {}", line);
    }
}

// Value editor: memory of this process, read and written with process_vm_readv/writev so that a bad
// address is an error and not a crash. (Opening /proc/self/mem is only allowed in debuggable apps.)
unsafe fn overlay_mem(env: *mut JNIEnv, address: jlong, array: jbyteArray, length: jint, write: bool) -> jint {
    if array.is_null() || length <= 0 || length > jni!(env, GetArrayLength, array) {
        return -libc::EINVAL;
    }
    let bytes = jni!(env, GetByteArrayElements, array, std::ptr::null_mut());
    if bytes.is_null() {
        return -libc::ENOMEM;
    }
    let local = libc::iovec { iov_base: bytes as *mut c_void, iov_len: length as usize };
    let remote = libc::iovec { iov_base: address as usize as *mut c_void, iov_len: length as usize };
    let n = if write {
        libc::process_vm_writev(libc::getpid(), &local, 1, &remote, 1, 0)
    } else {
        libc::process_vm_readv(libc::getpid(), &local, 1, &remote, 1, 0)
    };
    let result = if n < 0 { -crate::util::errno() } else { n as jint };
    jni!(env, ReleaseByteArrayElements, array, bytes, if write { JNI_ABORT } else { 0 });
    result
}
unsafe extern "system" fn overlay_mem_read(env: *mut JNIEnv, _cls: jclass, address: jlong, dst: jbyteArray, length: jint) -> jint {
    overlay_mem(env, address, dst, length, false)
}
unsafe extern "system" fn overlay_mem_write(env: *mut JNIEnv, _cls: jclass, address: jlong, src: jbyteArray, length: jint) -> jint {
    overlay_mem(env, address, src, length, true)
}

unsafe extern "system" fn overlay_fps(_env: *mut JNIEnv, _cls: jclass) -> jfloat {
    static LAST: Mutex<Option<(u64, Instant)>> = Mutex::new(None);
    let now = Instant::now();
    let mut frames = G_FRAMES.load(Ordering::Relaxed);
    if frames == 0 {
        frames = jni_frame_method_calls(); // frames presented from Java
    }
    let mut last = lock(&LAST);
    let (last_frames, last_time) = last.unwrap_or((0, now));
    let dt = now.duration_since(last_time).as_secs_f64();
    *last = Some((frames, now));
    if frames == 0 || dt <= 0.0 {
        -1.0
    } else {
        (frames.wrapping_sub(last_frames) as f64 / dt) as f32
    }
}

// The patcher puts the dex of the ARM64Bridge app into assets/arm64bridge/ (FPS display and in-game menu).
unsafe fn load_overlay(env: *mut JNIEnv) {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let Some(apk) = APK_PATH.get() else { return };
        // The dex buffers must stay alive: the class loader maps them.
        let mut dexes: Vec<&'static [u8]> = Vec::new();
        for i in 1..=16 {
            let name = if i == 1 { "classes.dex".to_string() } else { format!("classes{}.dex", i) };
            match zip_extract(apk, &format!("assets/arm64bridge/{}", name)) {
                Some(d) => dexes.push(Box::leak(d.into_boxed_slice())),
                None => break,
            }
        }
        if dexes.is_empty() {
            return;
        }
        let app = current_application(env);
        let bb_cls = jni!(env, FindClass, c"java/nio/ByteBuffer".as_ptr());
        let cl_cls = jni!(env, FindClass, c"java/lang/ClassLoader".as_ptr());
        let im_cls = jni!(env, FindClass, c"dalvik/system/InMemoryDexClassLoader".as_ptr());
        if clear_exc(env) || app.is_null() || bb_cls.is_null() || cl_cls.is_null() || im_cls.is_null() {
            logw!("in-game menu: not available on this Android version");
            return;
        }
        let buffers = jni!(env, NewObjectArray, dexes.len() as jsize, bb_cls, std::ptr::null_mut());
        for (i, d) in dexes.iter().enumerate() {
            let b = jni!(env, NewDirectByteBuffer, d.as_ptr() as *mut c_void, d.len() as jlong);
            jni!(env, SetObjectArrayElement, buffers, i as jsize, b);
            jni!(env, DeleteLocalRef, b);
        }
        let get_sys = jni!(env, GetStaticMethodID, cl_cls, c"getSystemClassLoader".as_ptr(), c"()Ljava/lang/ClassLoader;".as_ptr());
        let parent = jni!(env, CallStaticObjectMethodA, cl_cls, get_sys, NO_ARGS);
        let ctor = jni!(env, GetMethodID, im_cls, c"<init>".as_ptr(), c"([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V".as_ptr());
        let loader = if !clear_exc(env) && !ctor.is_null() {
            jni!(env, NewObjectA, im_cls, ctor, [jvalue { l: buffers }, jvalue { l: parent }].as_ptr())
        } else {
            std::ptr::null_mut()
        };
        if clear_exc(env) || loader.is_null() {
            logw!("in-game menu: could not load the menu code");
            return;
        }
        let cname = new_jstring(env, "com.mesflit.arm64bridge.overlay.GameOverlay");
        let load_class = jni!(env, GetMethodID, cl_cls, c"loadClass".as_ptr(), c"(Ljava/lang/String;)Ljava/lang/Class;".as_ptr());
        let overlay = jni!(env, CallObjectMethodA, loader, load_class, [jvalue { l: cname }].as_ptr());
        if clear_exc(env) || overlay.is_null() {
            logw!("in-game menu: menu class not found");
            return;
        }
        jni!(env, NewGlobalRef, loader);
        let overlay = jni!(env, NewGlobalRef, overlay);
        let natives = [
            (c"nativeSetLimiter", c"(I)V", overlay_set_limiter as *mut c_void),
            (c"nativeFps", c"()F", overlay_fps as *mut c_void),
            (c"nativeVersion", c"()Ljava/lang/String;", overlay_version as *mut c_void),
            (c"nativeMemRead", c"(J[BI)I", overlay_mem_read as *mut c_void),
            (c"nativeMemWrite", c"(J[BI)I", overlay_mem_write as *mut c_void),
            (c"nativeLogCrash", c"(Ljava/lang/String;)V", overlay_log_crash as *mut c_void),
        ]
        .map(|(n, s, f)| JNINativeMethod { name: n.as_ptr() as *mut c_char, signature: s.as_ptr() as *mut c_char, fnPtr: f });
        // The FPS booster controls are an option of the patcher (marker file in the APK).
        let booster = zip_extract(apk, "assets/arm64bridge/fps-booster").is_some();
        if !booster {
            G_FRAME_LIMITER.store(0, Ordering::Relaxed);
        }
        let editor = zip_extract(apk, "assets/arm64bridge/value-editor").is_some();
        let install = jni!(env, GetStaticMethodID, overlay, c"install".as_ptr(), c"(Landroid/app/Application;ZZ)V".as_ptr());
        if jni!(env, RegisterNatives, overlay, natives.as_ptr(), natives.len() as jint) != JNI_OK || clear_exc(env) || install.is_null() {
            clear_exc(env);
            logw!("in-game menu: could not bind the menu");
            return;
        }
        jni!(env, CallStaticVoidMethodA, overlay, install, [jvalue { l: app }, jvalue { z: booster as u8 }, jvalue { z: editor as u8 }].as_ptr());
        if clear_exc(env) {
            logw!("in-game menu: installation failed");
        } else {
            logi!("in-game menu installed (FPS booster {}, value editor {})", if booster { "on" } else { "off" }, if editor { "on" } else { "off" });
        }
    });
}

unsafe fn host_env(vm: *mut JavaVM) -> *mut JNIEnv {
    let mut env: *mut c_void = std::ptr::null_mut();
    if ((**vm).GetEnv.expect("GetEnv"))(vm, &mut env, JNI_VERSION_1_6) != JNI_OK {
        return std::ptr::null_mut();
    }
    env as *mut JNIEnv
}

/// Called from the JNI_OnLoad of a shim library (lib/arm64-v8a/libfoo.so inside a
/// patched APK). shim_path tells us which 32-bit library it stands in for.
#[no_mangle]
pub unsafe extern "C" fn a64b_shim_load(vm: *mut JavaVM, shim_path: *const c_char) -> jint {
    init_from_environment();
    let env = host_env(vm);
    if env.is_null() {
        return JNI_ERR;
    }
    open_log_file(env);
    crate::crash::crash_handler_install();
    crate::java_proc_hook::java_proc_hook_install();
    let path = cstr(shim_path);
    logi!("System.loadLibrary -> shim {}", path);
    let name = crate::util::base_name(&path).to_string();

    // Locate the APK that carries the 32-bit libraries.
    if let Some(bang) = path.find("!/") {
        add_guest_apk(&path[..bang]); // library is mapped straight from the APK
    } else {
        // <app dir>/lib/arm64/libfoo.so -> <app dir>/base.apk
        let dir = &path[..path.rfind('/').unwrap_or(0)];
        if let Ok(real) = std::fs::canonicalize(format!("{}/../../base.apk", dir)) {
            if let Some(real) = real.to_str() {
                if std::fs::metadata(real).is_ok() {
                    add_guest_apk(real);
                }
            }
        }
    }
    load_overlay(env);
    let result = jni_load_library(env, &name);
    logi!("{}: load finished ({})", name, if result.is_err() { "FAILED" } else { "ok" });
    match result {
        Err(error) => {
            loge!("ARM64Bridge could not load {}: {}", name, error);
            let ule = jni!(env, FindClass, c"java/lang/UnsatisfiedLinkError".as_ptr());
            if !ule.is_null() {
                let msg = cstring(&format!("ARM64Bridge: {}: {}", name, error));
                jni!(env, ThrowNew, ule, msg.as_ptr());
            }
            JNI_VERSION_1_6 // the pending exception reports the failure
        }
        Ok(v) => {
            if v >= JNI_VERSION_1_1 {
                v
            } else {
                JNI_VERSION_1_6
            }
        }
    }
}

// ----- com.mesflit.arm64bridge.NativeBridge -----

#[no_mangle]
pub unsafe extern "system" fn Java_com_mesflit_arm64bridge_NativeBridge_inspectElf(env: *mut JNIEnv, _cls: jclass, data: jbyteArray) -> jstring {
    let n = jni!(env, GetArrayLength, data);
    let mut buf = vec![0u8; n.max(0) as usize];
    jni!(env, GetByteArrayRegion, data, 0, n, buf.as_mut_ptr() as *mut jbyte);
    let info = elf_inspect(&buf);
    let mut out = String::new();
    out += &format!("valid={}\narm32={}\nmachine={}\n", info.valid as u8, info.arm32 as u8, info.machine);
    if !info.error.is_empty() {
        out += &format!("error={}\n", info.error);
    }
    for s in &info.needed {
        out += &format!("needed={}\n", s);
    }
    out += &format!("imports={}\nexports={}\n", info.imports.len(), info.exports.len());
    // Imports are only "missing" if neither the bridge nor (possibly) another bundled library has them;
    // the caller subtracts the exports of the sibling libraries.
    for s in &info.imports {
        if !thunk_exists(s) {
            out += &format!("unresolved={}\n", s);
        }
    }
    for s in &info.exports {
        out += &format!("export={}\n", s);
    }
    new_jstring(env, &out)
}

/// Called by the BridgeStarter provider of a patched app, before the app's own code runs.
#[no_mangle]
pub unsafe extern "system" fn Java_com_mesflit_arm64bridge_overlay_BridgeStarter_nativeStart(_env: *mut JNIEnv, _cls: jclass) {
    init_from_environment();
    crate::java_proc_hook::java_proc_hook_install();
}

#[no_mangle]
pub unsafe extern "system" fn Java_com_mesflit_arm64bridge_overlay_BridgeStarter_nativeLog(env: *mut JNIEnv, _cls: jclass, text: jstring) {
    logw!("{}", jstr(env, text));
}

#[no_mangle]
pub unsafe extern "system" fn Java_com_mesflit_arm64bridge_NativeBridge_addGuestDirectory(env: *mut JNIEnv, _cls: jclass, dir: jstring) {
    init_from_environment();
    crate::java_proc_hook::java_proc_hook_install(); // for the self test
    add_guest_dir(&jstr(env, dir));
}

#[no_mangle]
pub unsafe extern "system" fn Java_com_mesflit_arm64bridge_NativeBridge_loadGuestLibrary(env: *mut JNIEnv, _cls: jclass, name: jstring) -> jstring {
    init_from_environment();
    match jni_load_library(env, &jstr(env, name)) {
        Ok(_) => std::ptr::null_mut(),
        Err(e) => new_jstring(env, if e.is_empty() { "load failed" } else { &e }),
    }
}

#[no_mangle]
pub unsafe extern "system" fn Java_com_mesflit_arm64bridge_NativeBridge_version(env: *mut JNIEnv, _cls: jclass) -> jstring {
    new_jstring(env, &format!("ARM64Bridge core {} ({})", VERSION, crate::cpu::BACKEND_NAME))
}
