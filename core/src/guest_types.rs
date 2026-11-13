//! Layouts of 32-bit bionic structures and conversion helpers.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};
use std::sync::Mutex;

use crate::util::lock;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct GTimespec {
    pub tv_sec: i32,
    pub tv_nsec: i32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct GTimeval {
    pub tv_sec: i32,
    pub tv_usec: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct GTm {
    pub tm_sec: i32,
    pub tm_min: i32,
    pub tm_hour: i32,
    pub tm_mday: i32,
    pub tm_mon: i32,
    pub tm_year: i32,
    pub tm_wday: i32,
    pub tm_yday: i32,
    pub tm_isdst: i32,
    pub tm_gmtoff: i32,
    pub tm_zone: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GStat {
    pub st_dev: u64,
    pub pad0: [u8; 4],
    pub st_ino32: u32,
    pub st_mode: u32,
    pub st_nlink: u32,
    pub st_uid: u32,
    pub st_gid: u32,
    pub st_rdev: u64,
    pub pad3: [u8; 4],
    pub pad4: [u8; 4], // st_size is 8-byte aligned
    pub st_size: i64,
    pub st_blksize: u32,
    pub pad5: [u8; 4], // st_blocks is 8-byte aligned
    pub st_blocks: u64,
    pub st_atim: GTimespec,
    pub st_mtim: GTimespec,
    pub st_ctim: GTimespec,
    pub st_ino: u64,
}
const _: () = assert!(std::mem::size_of::<GStat>() == 104);
const _: () = assert!(std::mem::offset_of!(GStat, st_size) == 48);
const _: () = assert!(std::mem::offset_of!(GStat, st_blocks) == 64);
const _: () = assert!(std::mem::offset_of!(GStat, st_ino) == 96);

pub fn ts_to_guest(h: &libc::timespec) -> GTimespec {
    GTimespec { tv_sec: h.tv_sec as i32, tv_nsec: h.tv_nsec as i32 }
}
pub fn ts_from_guest(g: &GTimespec) -> libc::timespec {
    let mut t: libc::timespec = unsafe { std::mem::zeroed() };
    t.tv_sec = g.tv_sec as _;
    t.tv_nsec = g.tv_nsec as _;
    t
}

pub unsafe fn stat_to_guest(g: *mut GStat, s: &libc::stat) {
    let mut v: GStat = std::mem::zeroed();
    v.st_dev = s.st_dev as u64;
    v.st_ino32 = s.st_ino as u32;
    v.st_mode = s.st_mode as u32;
    v.st_nlink = s.st_nlink as u32;
    v.st_uid = s.st_uid;
    v.st_gid = s.st_gid;
    v.st_rdev = s.st_rdev as u64;
    v.st_size = s.st_size as i64;
    v.st_blksize = s.st_blksize as u32;
    v.st_blocks = s.st_blocks as u64;
    v.st_atim = GTimespec { tv_sec: s.st_atime as i32, tv_nsec: s.st_atime_nsec as i32 };
    v.st_mtim = GTimespec { tv_sec: s.st_mtime as i32, tv_nsec: s.st_mtime_nsec as i32 };
    v.st_ctim = GTimespec { tv_sec: s.st_ctime as i32, tv_nsec: s.st_ctime_nsec as i32 };
    v.st_ino = s.st_ino as u64;
    std::ptr::write_unaligned(g, v);
}

pub fn tm_to_guest(t: &libc::tm, zone: u32) -> GTm {
    GTm {
        tm_sec: t.tm_sec,
        tm_min: t.tm_min,
        tm_hour: t.tm_hour,
        tm_mday: t.tm_mday,
        tm_mon: t.tm_mon,
        tm_year: t.tm_year,
        tm_wday: t.tm_wday,
        tm_yday: t.tm_yday,
        tm_isdst: t.tm_isdst,
        tm_gmtoff: t.tm_gmtoff as i32,
        tm_zone: zone,
    }
}
pub fn tm_from_guest(g: &GTm) -> libc::tm {
    let mut t: libc::tm = unsafe { std::mem::zeroed() };
    t.tm_sec = g.tm_sec;
    t.tm_min = g.tm_min;
    t.tm_hour = g.tm_hour;
    t.tm_mday = g.tm_mday;
    t.tm_mon = g.tm_mon;
    t.tm_year = g.tm_year;
    t.tm_wday = g.tm_wday;
    t.tm_yday = g.tm_yday;
    t.tm_isdst = g.tm_isdst;
    t.tm_gmtoff = g.tm_gmtoff as _;
    t
}

/// Growable table of host objects addressed by small integers, with lock-free lookup
/// (used for objects whose guest representation is only 4 bytes wide).
pub struct HandleTable<T> {
    mu: Mutex<(u32, Vec<u32>)>, // next index, free list
    chunks: [AtomicPtr<AtomicPtr<T>>; TOP],
}
const CHUNK_BITS: u32 = 12;
const CHUNK: usize = 1 << CHUNK_BITS;
const TOP: usize = 1 << 16;

impl<T> HandleTable<T> {
    pub const fn new() -> Self {
        HandleTable { mu: Mutex::new((1, Vec::new())), chunks: [const { AtomicPtr::new(std::ptr::null_mut()) }; TOP] } // index 0 is never used
    }
    pub fn put(&self, obj: *mut T) -> u32 {
        let idx;
        {
            let mut g = lock(&self.mu);
            if let Some(i) = g.1.pop() {
                idx = i;
            } else {
                idx = g.0;
                g.0 += 1;
                let ci = (idx >> CHUNK_BITS) as usize;
                if ci >= TOP {
                    fatal!("handle table full");
                }
                if self.chunks[ci].load(Ordering::SeqCst).is_null() {
                    let chunk: Box<[AtomicPtr<T>]> = (0..CHUNK).map(|_| AtomicPtr::new(std::ptr::null_mut())).collect();
                    self.chunks[ci].store(Box::leak(chunk).as_mut_ptr(), Ordering::SeqCst);
                }
            }
        }
        unsafe { (*self.slot(idx)).store(obj, Ordering::SeqCst) };
        idx
    }
    unsafe fn slot(&self, idx: u32) -> *const AtomicPtr<T> {
        self.chunks[(idx >> CHUNK_BITS) as usize].load(Ordering::Acquire).add(idx as usize & (CHUNK - 1))
    }
    pub fn get(&self, idx: u32) -> *mut T {
        if (idx >> CHUNK_BITS) as usize >= TOP {
            return std::ptr::null_mut();
        }
        let ch = self.chunks[(idx >> CHUNK_BITS) as usize].load(Ordering::Acquire);
        if ch.is_null() {
            return std::ptr::null_mut();
        }
        unsafe { (*ch.add(idx as usize & (CHUNK - 1))).load(Ordering::Acquire) }
    }
    pub fn take(&self, idx: u32) -> *mut T {
        let o = self.get(idx);
        if o.is_null() {
            return o;
        }
        unsafe { (*self.slot(idx)).store(std::ptr::null_mut(), Ordering::SeqCst) };
        lock(&self.mu).1.push(idx);
        o
    }
}
impl<T> Default for HandleTable<T> {
    fn default() -> Self {
        Self::new()
    }
}
unsafe impl<T> Sync for HandleTable<T> {}
unsafe impl<T> Send for HandleTable<T> {}

/// A counter that hands out the first n "true"s (for messages that are only logged a few times).
pub struct LogLimit(AtomicUsize);
impl LogLimit {
    pub const fn new() -> Self {
        LogLimit(AtomicUsize::new(0))
    }
    pub fn below(&self, n: usize) -> bool {
        self.0.fetch_add(1, Ordering::Relaxed) < n
    }
}
impl Default for LogLimit {
    fn default() -> Self {
        Self::new()
    }
}
