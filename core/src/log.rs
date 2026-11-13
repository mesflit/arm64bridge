//! Logging: logcat (or stderr on a desktop), an optional log file and an optional capture buffer.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Mutex;

pub const LOG_DEBUG: i32 = 3;
pub const LOG_INFO: i32 = 4;
pub const LOG_WARN: i32 = 5;
pub const LOG_ERROR: i32 = 6;

pub static LOG_LEVEL: AtomicI32 = AtomicI32::new(LOG_INFO);
static FILE_FD: AtomicI32 = AtomicI32::new(-1);
static CAPTURE: Mutex<Option<String>> = Mutex::new(None);

#[macro_export]
macro_rules! logd {
    ($($arg:tt)*) => {
        if $crate::log::LOG_LEVEL.load(std::sync::atomic::Ordering::Relaxed) <= $crate::log::LOG_DEBUG {
            $crate::log::write($crate::log::LOG_DEBUG, &format!($($arg)*));
        }
    };
}
#[macro_export]
macro_rules! logi {
    ($($arg:tt)*) => { $crate::log::write($crate::log::LOG_INFO, &format!($($arg)*)) };
}
#[macro_export]
macro_rules! logw {
    ($($arg:tt)*) => { $crate::log::write($crate::log::LOG_WARN, &format!($($arg)*)) };
}
#[macro_export]
macro_rules! loge {
    ($($arg:tt)*) => { $crate::log::write($crate::log::LOG_ERROR, &format!($($arg)*)) };
}
/// Logs the message as an error and aborts the process.
#[macro_export]
macro_rules! fatal {
    ($($arg:tt)*) => { $crate::log::fatal(&format!($($arg)*)) };
}

/// Mirrors every log line into an open file descriptor (-1 disables).
pub fn set_file(fd: i32) {
    FILE_FD.store(fd, Ordering::SeqCst);
}
pub fn file_fd() -> i32 {
    FILE_FD.load(Ordering::SeqCst)
}

/// Starts (true) or stops (false) keeping a copy of every log line.
pub fn set_capture(on: bool) {
    *CAPTURE.lock().unwrap_or_else(|e| e.into_inner()) = if on { Some(String::new()) } else { None };
}
/// The lines captured so far.
pub fn take_capture() -> String {
    CAPTURE.lock().unwrap_or_else(|e| e.into_inner()).as_mut().map(std::mem::take).unwrap_or_default()
}

// One line in the log file: "MM-DD hh:mm:ss.mmm tid L tag: message". Uses only write(),
// so a line is never lost to buffering when the process dies right after.
fn file_line(level: i32, tag: &str, msg: &str) {
    let fd = FILE_FD.load(Ordering::Relaxed);
    if fd < 0 {
        return;
    }
    const LV: &[u8; 8] = b"??VDIWEF";
    unsafe {
        let mut tv: libc::timeval = std::mem::zeroed();
        libc::gettimeofday(&mut tv, std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        let t: libc::time_t = tv.tv_sec;
        libc::localtime_r(&t, &mut tm);
        let line = format!(
            "{:02}-{:02} {:02}:{:02}:{:02}.{:03} {:5} {} {}: {}\n",
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            tv.tv_usec / 1000,
            crate::util::gettid(),
            LV[(level & 7) as usize] as char,
            tag,
            msg
        );
        let _ = libc::write(fd, line.as_ptr() as *const libc::c_void, line.len());
    }
}

#[cfg(target_os = "android")]
extern "C" {
    fn __android_log_write(prio: libc::c_int, tag: *const libc::c_char, text: *const libc::c_char) -> libc::c_int;
}

fn emit(level: i32, tag: &str, msg: &str) {
    #[cfg(target_os = "android")]
    unsafe {
        let t = crate::util::cstring(tag);
        let m = crate::util::cstring(msg);
        __android_log_write(level, t.as_ptr(), m.as_ptr());
    }
    #[cfg(not(target_os = "android"))]
    {
        use std::io::Write;
        const NAMES: [&str; 8] = ["?", "?", "V", "D", "I", "W", "E", "F"];
        let line = if tag == "arm64bridge" {
            format!("[a64b {}] {}\n", NAMES[(level & 7) as usize], msg)
        } else {
            format!("[{} {}] {}\n", tag, level, msg)
        };
        let _ = std::io::stderr().write_all(line.as_bytes());
    }
    file_line(level, tag, msg);
}

/// Log a line under another tag (used for messages written by guest code).
pub fn tagged(level: i32, tag: &str, msg: &str) {
    emit(level, tag, msg);
}

pub fn write(level: i32, msg: &str) {
    // Like the fixed buffer of a C logger: very long lines are cut.
    let mut end = msg.len().min(2047);
    while !msg.is_char_boundary(end) {
        end -= 1;
    }
    let msg = &msg[..end];
    emit(level, "arm64bridge", msg);
    if let Ok(mut cap) = CAPTURE.try_lock() {
        if let Some(s) = cap.as_mut() {
            s.push_str(msg);
            s.push('\n');
        }
    }
}

pub fn fatal(msg: &str) -> ! {
    write(LOG_ERROR, msg);
    unsafe { libc::abort() }
}
