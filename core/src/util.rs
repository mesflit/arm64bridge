//! Small helpers shared by the whole runtime.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::cell::UnsafeCell;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;

/// errno of the calling thread.
#[inline]
pub fn errno() -> i32 {
    unsafe { *errno_location() }
}
#[inline]
pub fn set_errno(v: i32) {
    unsafe { *errno_location() = v }
}
#[inline]
unsafe fn errno_location() -> *mut i32 {
    #[cfg(target_os = "android")]
    {
        libc::__errno()
    }
    #[cfg(not(target_os = "android"))]
    {
        libc::__errno_location()
    }
}

#[inline]
pub fn gettid() -> i32 {
    unsafe { libc::syscall(libc::SYS_gettid) as i32 }
}

/// A C string for a host API; interior NUL bytes end the string.
pub fn cstring(s: &str) -> CString {
    match CString::new(s) {
        Ok(c) => c,
        Err(e) => {
            let n = e.nul_position();
            CString::new(&s.as_bytes()[..n]).unwrap()
        }
    }
}

/// Text of a NUL-terminated string (empty for NULL), with invalid UTF-8 replaced.
pub unsafe fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// Bytes of a NUL-terminated string (without the terminator).
pub unsafe fn cbytes<'a>(p: *const c_char) -> &'a [u8] {
    if p.is_null() {
        &[]
    } else {
        CStr::from_ptr(p).to_bytes()
    }
}

/// File name part of a path.
pub fn base_name(path: &str) -> &str {
    match path.rfind('/') {
        Some(i) => &path[i + 1..],
        None => path,
    }
}

/// A mutex the owning thread may lock again (the loader calls back into itself through guest code).
pub struct RecMutex {
    inner: UnsafeCell<libc::pthread_mutex_t>,
    init: std::sync::Once,
}
unsafe impl Sync for RecMutex {}
unsafe impl Send for RecMutex {}

pub struct RecGuard<'a>(&'a RecMutex);

impl RecMutex {
    pub const fn new() -> Self {
        RecMutex { inner: UnsafeCell::new(libc::PTHREAD_MUTEX_INITIALIZER), init: std::sync::Once::new() }
    }
    fn raw(&self) -> *mut libc::pthread_mutex_t {
        self.init.call_once(|| unsafe {
            let mut attr: libc::pthread_mutexattr_t = std::mem::zeroed();
            libc::pthread_mutexattr_init(&mut attr);
            libc::pthread_mutexattr_settype(&mut attr, libc::PTHREAD_MUTEX_RECURSIVE);
            libc::pthread_mutex_init(self.inner.get(), &attr);
            libc::pthread_mutexattr_destroy(&mut attr);
        });
        self.inner.get()
    }
    pub fn lock(&self) -> RecGuard<'_> {
        unsafe { libc::pthread_mutex_lock(self.raw()) };
        RecGuard(self)
    }
    pub fn try_lock(&self) -> Option<RecGuard<'_>> {
        if unsafe { libc::pthread_mutex_trylock(self.raw()) } == 0 {
            Some(RecGuard(self))
        } else {
            None
        }
    }
    /// Locks without a guard; pair with `unlock_raw`.
    pub fn lock_raw(&self) {
        unsafe { libc::pthread_mutex_lock(self.raw()) };
    }
    pub fn unlock_raw(&self) {
        unsafe { libc::pthread_mutex_unlock(self.raw()) };
    }
}
impl Default for RecMutex {
    fn default() -> Self {
        Self::new()
    }
}
impl Drop for RecGuard<'_> {
    fn drop(&mut self) {
        unsafe { libc::pthread_mutex_unlock(self.0.inner.get()) };
    }
}

/// Locks a std mutex, ignoring poisoning (a panic elsewhere must not take the bridge down with it).
pub fn lock<T>(m: &std::sync::Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// A value that is only touched under an external lock or by one thread (documented at each use).
pub struct Racy<T>(pub UnsafeCell<T>);
unsafe impl<T> Sync for Racy<T> {}
impl<T> Racy<T> {
    pub const fn new(v: T) -> Self {
        Racy(UnsafeCell::new(v))
    }
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn get(&self) -> &mut T {
        &mut *self.0.get()
    }
}
