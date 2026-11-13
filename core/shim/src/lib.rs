//! Shim library. The patcher places one copy of this library in lib/arm64-v8a/ for
//! every 32-bit library of the original app, under the original file name. When
//! the app calls System.loadLibrary("foo"), Android loads this 64-bit shim, whose
//! JNI_OnLoad asks the bridge runtime (libarm64bridge.so, next to it) to load and
//! run the real 32-bit libfoo.so.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::ffi::c_void;
use std::os::raw::{c_char, c_int};

const JNI_ERR: c_int = -1;
type ShimLoad = unsafe extern "C" fn(vm: *mut c_void, shim_path: *const c_char) -> c_int;

#[no_mangle]
pub unsafe extern "system" fn JNI_OnLoad(vm: *mut c_void, _reserved: *mut c_void) -> c_int {
    let mut info: libc::Dl_info = std::mem::zeroed();
    if libc::dladdr(JNI_OnLoad as *const c_void, &mut info) == 0 || info.dli_fname.is_null() {
        return JNI_ERR;
    }
    // The runtime lives in the same directory (or APK) as this shim; the loader finds it by name.
    let rt = libc::dlopen(c"libarm64bridge.so".as_ptr(), libc::RTLD_NOW | libc::RTLD_GLOBAL);
    if rt.is_null() {
        return JNI_ERR;
    }
    let f = libc::dlsym(rt, c"a64b_shim_load".as_ptr());
    if f.is_null() {
        return JNI_ERR;
    }
    std::mem::transmute::<*mut c_void, ShimLoad>(f)(vm, info.dli_fname)
}
