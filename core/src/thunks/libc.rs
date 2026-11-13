//! Host implementations of the bionic libc functions imported by guest libraries.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::collections::HashMap;
use std::os::raw::{c_char, c_int, c_long, c_void};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use crate::cpu::{BlockingCall, Cpu, G_FRAME_LIMITER, G_RENDER_TID};
use crate::elf::describe_address;
use crate::frame_cap::{frame_cap_known, frame_cap_on_clock, frame_cap_on_sleep};
use crate::guest_types::*;
use crate::limiter_probe::probe_time_call;
use crate::mem::*;
use crate::proc::guest_proc_open;
use crate::thunk::{reg, reg_raw, ArgReader};
use crate::util::{cstr, errno, lock, set_errno};

type Str = *const c_char;
type MStr = *mut c_char;
type P = *mut c_void;
type CP = *const c_void;
type W = *mut u32; // wchar_t* (32 bits wide in the guest and on the host)

/// Host C library functions the `libc` crate does not declare for every target.
pub mod sys {
    use std::os::raw::{c_char, c_double, c_float, c_int, c_long, c_longlong, c_uint, c_ulonglong, c_void};
    extern "C" {
        pub fn memrchr(s: *const c_void, c: c_int, n: usize) -> *mut c_void;
        pub fn memmem(h: *const c_void, hl: usize, n: *const c_void, nl: usize) -> *mut c_void;
        pub fn memccpy(d: *mut c_void, s: *const c_void, c: c_int, n: usize) -> *mut c_void;
        pub fn stpcpy(d: *mut c_char, s: *const c_char) -> *mut c_char;
        pub fn strxfrm(d: *mut c_char, s: *const c_char, n: usize) -> usize;
        pub fn strcasestr(h: *const c_char, n: *const c_char) -> *mut c_char;
        pub fn strtok(s: *mut c_char, d: *const c_char) -> *mut c_char;
        pub fn strtok_r(s: *mut c_char, d: *const c_char, save: *mut *mut c_char) -> *mut c_char;
        pub fn strsep(s: *mut *mut c_char, d: *const c_char) -> *mut c_char;
        pub fn strsignal(s: c_int) -> *mut c_char;
        pub fn strtol(s: *const c_char, e: *mut *mut c_char, base: c_int) -> c_long;
        pub fn strtoll(s: *const c_char, e: *mut *mut c_char, base: c_int) -> c_longlong;
        pub fn strtoull(s: *const c_char, e: *mut *mut c_char, base: c_int) -> c_ulonglong;
        pub fn strtod(s: *const c_char, e: *mut *mut c_char) -> c_double;
        pub fn strtof(s: *const c_char, e: *mut *mut c_char) -> c_float;
        pub fn atoi(s: *const c_char) -> c_int;
        pub fn atoll(s: *const c_char) -> c_longlong;
        pub fn atof(s: *const c_char) -> c_double;
        pub fn isblank(c: c_int) -> c_int;
        pub fn iswalpha(c: c_uint) -> c_int;
        pub fn iswdigit(c: c_uint) -> c_int;
        pub fn iswalnum(c: c_uint) -> c_int;
        pub fn iswspace(c: c_uint) -> c_int;
        pub fn iswupper(c: c_uint) -> c_int;
        pub fn iswlower(c: c_uint) -> c_int;
        pub fn iswxdigit(c: c_uint) -> c_int;
        pub fn iswpunct(c: c_uint) -> c_int;
        pub fn iswprint(c: c_uint) -> c_int;
        pub fn iswcntrl(c: c_uint) -> c_int;
        pub fn towlower(c: c_uint) -> c_uint;
        pub fn towupper(c: c_uint) -> c_uint;
        pub fn wcslen(s: *const u32) -> usize;
        pub fn wcscpy(d: *mut u32, s: *const u32) -> *mut u32;
        pub fn wcsncpy(d: *mut u32, s: *const u32, n: usize) -> *mut u32;
        pub fn wcscat(d: *mut u32, s: *const u32) -> *mut u32;
        pub fn wcscmp(a: *const u32, b: *const u32) -> c_int;
        pub fn wcsncmp(a: *const u32, b: *const u32, n: usize) -> c_int;
        pub fn wcschr(s: *const u32, c: u32) -> *mut u32;
        pub fn wcsrchr(s: *const u32, c: u32) -> *mut u32;
        pub fn wcsstr(h: *const u32, n: *const u32) -> *mut u32;
        pub fn wmemcpy(d: *mut u32, s: *const u32, n: usize) -> *mut u32;
        pub fn wmemmove(d: *mut u32, s: *const u32, n: usize) -> *mut u32;
        pub fn wmemset(d: *mut u32, c: u32, n: usize) -> *mut u32;
        pub fn wmemcmp(a: *const u32, b: *const u32, n: usize) -> c_int;
        pub fn wmemchr(s: *const u32, c: u32, n: usize) -> *mut u32;
        pub fn mbstowcs(d: *mut u32, s: *const c_char, n: usize) -> usize;
        pub fn wcstombs(d: *mut c_char, s: *const u32, n: usize) -> usize;
        pub fn mbtowc(d: *mut u32, s: *const c_char, n: usize) -> c_int;
        pub fn wctomb(d: *mut c_char, c: u32) -> c_int;
        pub fn mblen(s: *const c_char, n: usize) -> c_int;
        pub fn btowc(c: c_int) -> c_uint;
        pub fn wctob(c: c_uint) -> c_int;
        pub fn mbrtowc(d: *mut u32, s: *const c_char, n: usize, st: *mut c_void) -> usize;
        pub fn wcrtomb(d: *mut c_char, c: u32, st: *mut c_void) -> usize;
        pub fn mbrlen(s: *const c_char, n: usize, st: *mut c_void) -> usize;
        pub fn __ctype_get_mb_cur_max() -> usize;
        pub fn srandom(seed: c_uint);
        pub fn random() -> c_long;
        pub fn rand_r(seed: *mut c_uint) -> c_int;
        pub fn lrand48() -> c_long;
        pub fn mrand48() -> c_long;
        pub fn drand48() -> c_double;
        pub fn srand48(seed: c_long);
        pub fn clock() -> c_long;
        pub fn asctime_r(tm: *const libc::tm, buf: *mut c_char) -> *mut c_char;
        pub fn ctime_r(t: *const libc::time_t, buf: *mut c_char) -> *mut c_char;
        pub fn timegm(tm: *mut libc::tm) -> libc::time_t;
        pub fn tzset();
        pub fn fnmatch(pattern: *const c_char, s: *const c_char, flags: c_int) -> c_int;
        pub fn nice(inc: c_int) -> c_int;
        pub fn clock_nanosleep(clk: c_int, flags: c_int, req: *const libc::timespec, rem: *mut libc::timespec) -> c_int;
    }
}

// ---- string helpers that hand host-owned strings to the guest ----
static STR_CACHE: Mutex<Option<HashMap<Vec<u8>, u32>>> = Mutex::new(None);

/// A cached copy of the bytes (plus NUL) in guest memory.
pub fn intern_bytes(s: &[u8]) -> u32 {
    let mut g = lock(&STR_CACHE);
    let cache = g.get_or_insert_with(HashMap::new);
    if let Some(&a) = cache.get(s) {
        return a;
    }
    let a = g_strdup_bytes(s);
    cache.insert(s.to_vec(), a);
    a
}
pub fn intern_str(s: &str) -> u32 {
    intern_bytes(s.as_bytes())
}
/// Host-owned C string handed to the guest (cached copy in guest memory); NULL stays NULL.
pub fn intern_cstr(s: *const c_char) -> u32 {
    if s.is_null() {
        return 0;
    }
    intern_bytes(unsafe { crate::util::cbytes(s) })
}

// ---- in-place heap sort calling a guest comparator ----
unsafe fn guest_qsort(c: &mut Cpu, base: u32, n: u32, size: u32, cmp: u32) {
    if n < 2 || size == 0 {
        return;
    }
    let b = g2h::<u8>(base);
    let sz = size as usize;
    let mut less = |c: &mut Cpu, i: u32, j: u32| (c.call(cmp, &[base + i * size, base + j * size]) as i32) < 0;
    let swap = |i: u32, j: u32| std::ptr::swap_nonoverlapping(b.add(i as usize * sz), b.add(j as usize * sz), sz);
    let mut sift = |c: &mut Cpu, start: u32, end: u32| {
        let mut root = start;
        while root * 2 + 1 <= end {
            let child = root * 2 + 1;
            let mut sw = root;
            if less(c, sw, child) {
                sw = child;
            }
            if child + 1 <= end && less(c, sw, child + 1) {
                sw = child + 1;
            }
            if sw == root {
                return;
            }
            swap(root, sw);
            root = sw;
        }
    };
    let mut start = (n - 2) / 2 + 1;
    while start > 0 {
        start -= 1;
        sift(c, start, n - 1);
    }
    let mut end = n - 1;
    while end > 0 {
        swap(0, end);
        sift(c, 0, end - 1);
        end -= 1;
    }
}

pub fn translate_open_flags(gf: i32) -> i32 {
    if cfg!(any(target_arch = "aarch64", target_arch = "arm")) {
        return gf;
    }
    // ARM uses different values for a few flags than the generic ABI.
    let mut hf = gf & !(0o40000 | 0o100000 | 0o200000 | 0o400000);
    if gf & 0o40000 != 0 {
        hf |= libc::O_DIRECTORY;
    }
    if gf & 0o100000 != 0 {
        hf |= libc::O_NOFOLLOW;
    }
    if gf & 0o400000 != 0 {
        hf |= libc::O_LARGEFILE;
    }
    hf
}

pub unsafe fn guest_open(p: Str, fl: i32, mode: i32) -> i32 {
    let v = guest_proc_open(p);
    if v != -2 {
        v
    } else {
        libc::open(p, translate_open_flags(fl), mode as libc::c_uint)
    }
}

struct GuestDir {
    dir: usize, // DIR*
    entry: u32, // guest buffer for the returned dirent
}
static DIRS: Mutex<Option<HashMap<u32, GuestDir>>> = Mutex::new(None);
fn find_dir(h: u32) -> Option<(*mut libc::DIR, u32)> {
    lock(&DIRS).as_ref().and_then(|m| m.get(&h)).map(|d| (d.dir as *mut libc::DIR, d.entry))
}

static TM_BUF: AtomicU32 = AtomicU32::new(0);
static CTIME_BUF: AtomicU32 = AtomicU32::new(0);
fn lazy_buf(slot: &AtomicU32, size: u32) -> u32 {
    let mut b = slot.load(Ordering::Relaxed);
    if b == 0 {
        b = g_calloc(1, size);
        slot.store(b, Ordering::Relaxed);
    }
    b
}

unsafe fn do_mmap(c: &mut Cpu, offset_bytes: i64, addr: u32, len: u32, prot: i32, flags: i32, fd: i32) {
    static FAILURES: LogLimit = LogLimit::new();
    if len == 0 || len > 0xF000_0000 {
        if FAILURES.below(8) {
            logw!("mmap(0x{:08x}, {}, prot {}, flags 0x{:x}, fd {}): invalid length", addr, len, prot, flags, fd);
        }
        set_errno(libc::EINVAL);
        c.set_ret32(0xFFFF_FFFF);
        return;
    }
    let mut res = 0u32;
    let fixed = (flags & libc::MAP_FIXED) != 0;
    if addr != 0 {
        // A requested address: libraries that load other libraries themselves depend on it.
        res = page_map_at(addr, len, prot, flags, if (flags & libc::MAP_ANONYMOUS) != 0 { -1 } else { fd }, offset_bytes, fixed);
        static LOGGED: LogLimit = LogLimit::new();
        if LOGGED.below(24) {
            logi!(
                "mmap(0x{:08x}, {}, prot {}, flags 0x{:x}{}) = {}",
                addr,
                len,
                prot,
                flags,
                if fixed { " FIXED" } else { "" },
                if res != 0 { "at that address" } else if fixed { "FAILED" } else { "elsewhere" }
            );
        }
        if res == 0 && fixed {
            set_errno(libc::ENOMEM);
            c.set_ret32(0xFFFF_FFFF);
            return;
        }
    }
    if res != 0 {
    } else if (flags & libc::MAP_ANONYMOUS) != 0 {
        res = page_alloc(len, (prot & libc::PROT_EXEC) != 0);
    } else {
        let hflags = if (flags & libc::MAP_SHARED) != 0 { libc::MAP_SHARED } else { libc::MAP_PRIVATE };
        res = page_map_file(len, prot, hflags, fd, offset_bytes);
    }
    if res == 0 {
        if FAILURES.below(8) {
            logw!("mmap(0x{:08x}, {}, prot {}, flags 0x{:x}, fd {}, offset {}) failed", addr, len, prot, flags, fd, offset_bytes);
        }
        set_errno(libc::ENOMEM);
        res = 0xFFFF_FFFF;
    }
    // The first calls in full, with the code of the caller: a library loader inside a game is picky
    // about what mmap returns, and this shows what it checks.
    static SHOWN: LogLimit = LogLimit::new();
    if SHOWN.below(4) {
        let lr = c.r(14);
        logi!(
            "mmap(0x{:08x}, {}, prot {}, flags 0x{:x}, fd {}, offset {}) = 0x{:08x}, called from {} ({})",
            addr,
            len,
            prot,
            flags,
            fd,
            offset_bytes,
            res,
            describe_address(lr & !1),
            if lr & 1 != 0 { "thumb" } else { "arm" }
        );
        crate::limiter_probe::dump_guest_code(lr & !1, 192, 320);
    }
    c.set_ret32(res);
}

// The in-game menu can relax the game's own frame limiter: sleeps of the thread that presents
// frames are shortened or skipped.
fn limiter_mode_for_this_thread() -> i32 {
    let mode = G_FRAME_LIMITER.load(Ordering::Relaxed);
    // A known clock-based limiter is handled by changing its frame time (frame_cap.rs).
    if mode == 0 || frame_cap_known() {
        return 0;
    }
    thread_local! { static TID: i32 = crate::util::gettid(); }
    if TID.with(|t| *t) == G_RENDER_TID.load(Ordering::Relaxed) {
        mode
    } else {
        0
    }
}

// number parsing (char** end pointers are 4 bytes wide in the guest)
unsafe fn with_strto_end(c: &mut Cpu, f: impl FnOnce(&mut Cpu, &mut ArgReader, Str, *mut MStr)) {
    let mut a = ArgReader::new(c);
    let s: Str = a.ptr();
    let endp = a.next32();
    let mut end: MStr = std::ptr::null_mut();
    f(c, &mut a, s, &mut end);
    if endp != 0 {
        wr::<u32>(endp, h2g(end));
    }
}

macro_rules! r {
    ($name:expr, |$($a:ident : $t:ty),*| -> $ret:ty $body:block) => {
        reg($name, move |$($a: $t),*| -> $ret { unsafe { $body } })
    };
    ($name:expr, |$($a:ident : $t:ty),*| $body:block) => {
        reg($name, move |$($a: $t),*| -> () { unsafe { $body } })
    };
}

pub fn register_libc_thunks() {
    reg_memory();
    reg_string();
    reg_ctype();
    reg_stdlib();
    reg_time();
    reg_files();
    reg_net_and_signals();
}

fn reg_memory() {
    reg("malloc", |n: u32| -> u32 { g_malloc(n) });
    reg("calloc", |n: u32, s: u32| -> u32 { g_calloc(n, s) });
    reg("realloc", |p: u32, n: u32| -> u32 { g_realloc(p, n) });
    reg("free", |p: u32| g_free(p));
    reg("memalign", |al: u32, n: u32| -> u32 { g_memalign(al, n) });
    reg("aligned_alloc", |al: u32, n: u32| -> u32 { g_memalign(al, n) });
    reg("valloc", |n: u32| -> u32 { g_memalign(4096, n) });
    r!("posix_memalign", |out: *mut u32, al: u32, n: u32| -> i32 {
        let p = g_memalign(al, n);
        if p == 0 {
            return libc::ENOMEM;
        }
        *out = p;
        0
    });
    reg("malloc_usable_size", |p: u32| -> u32 { g_usable_size(p) });
    reg("dlmalloc_usable_size", |p: u32| -> u32 { g_usable_size(p) });
    // operator new / delete (system libstdc++)
    for n in ["_Znwj", "_Znaj", "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t"] {
        reg(n, |size: u32| -> u32 {
            let p = g_malloc(size);
            if p == 0 {
                fatal!("guest operator new: out of memory ({} bytes)", size);
            }
            p
        });
    }
    for n in ["_ZdlPv", "_ZdaPv", "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t", "_ZdlPvj", "_ZdaPvj"] {
        reg(n, |p: u32| g_free(p));
    }

    reg_raw("mmap", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (addr, len, prot, flags, fd) = (a.next32(), a.next32(), a.i32(), a.i32(), a.i32());
        let off = a.i32() as i64;
        do_mmap(c, off, addr, len, prot, flags, fd);
    });
    reg_raw("mmap64", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (addr, len, prot, flags, fd) = (a.next32(), a.next32(), a.i32(), a.i32(), a.i32());
        let off = a.next64() as i64;
        do_mmap(c, off, addr, len, prot, flags, fd);
    });
    reg_raw("__mmap2", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (addr, len, prot, flags, fd) = (a.next32(), a.next32(), a.i32(), a.i32(), a.i32());
        let off = a.next32() as i64 * 4096;
        do_mmap(c, off, addr, len, prot, flags, fd);
    });
    reg("munmap", |addr: u32, len: u32| -> i32 {
        if page_free(addr, len) {
            return 0;
        }
        set_errno(libc::EINVAL);
        -1
    });
    reg("mprotect", |addr: u32, len: u32, prot: i32| -> i32 {
        if prot & libc::PROT_EXEC != 0 {
            page_mark_exec(addr, len);
            mem_invalidate_code(addr, len);
        }
        0
    });
    reg("madvise", |_a: u32, _l: u32, _f: i32| -> i32 { 0 });
    reg("mlock", |_a: u32, _l: u32| -> i32 { 0 });
    reg("munlock", |_a: u32, _l: u32| -> i32 { 0 });
    r!("msync", |p: P, n: usize, f: i32| -> i32 { libc::msync(p, n, f) });
    reg("cacheflush", |start: u32, end: u32, _f: i32| -> i32 {
        mem_invalidate_code(start, end.saturating_sub(start));
        0
    });
    reg("__clear_cache", |start: u32, end: u32| mem_invalidate_code(start, end.saturating_sub(start)));
}

fn reg_string() {
    r!("memcpy", |d: P, s: CP, n: usize| -> P { libc::memcpy(d, s, n) });
    r!("memmove", |d: P, s: CP, n: usize| -> P { libc::memmove(d, s, n) });
    r!("memset", |d: P, v: i32, n: usize| -> P { libc::memset(d, v, n) });
    r!("memcmp", |x: CP, y: CP, n: usize| -> i32 { libc::memcmp(x, y, n) });
    r!("bcmp", |x: CP, y: CP, n: usize| -> i32 { libc::memcmp(x, y, n) });
    r!("bcopy", |s: CP, d: P, n: usize| {
        libc::memmove(d, s, n);
    });
    r!("bzero", |d: P, n: usize| {
        libc::memset(d, 0, n);
    });
    r!("memchr", |s: CP, ch: i32, n: usize| -> P { libc::memchr(s, ch, n) });
    r!("memrchr", |s: CP, ch: i32, n: usize| -> P { sys::memrchr(s, ch, n) });
    r!("memmem", |h: CP, hl: usize, n: CP, nl: usize| -> P { sys::memmem(h, hl, n, nl) });
    r!("memccpy", |d: P, s: CP, ch: i32, n: usize| -> P { sys::memccpy(d, s, ch, n) });
    r!("__memcpy_chk", |d: P, s: CP, n: usize, _l: usize| -> P { libc::memcpy(d, s, n) });
    r!("__memmove_chk", |d: P, s: CP, n: usize, _l: usize| -> P { libc::memmove(d, s, n) });
    r!("__memset_chk", |d: P, v: i32, n: usize, _l: usize| -> P { libc::memset(d, v, n) });
    r!("strlen", |s: Str| -> usize { libc::strlen(s) });
    r!("__strlen_chk", |s: Str, _l: usize| -> usize { libc::strlen(s) });
    r!("strnlen", |s: Str, n: usize| -> usize { libc::strnlen(s, n) });
    r!("strcpy", |d: MStr, s: Str| -> MStr { libc::strcpy(d, s) });
    r!("__strcpy_chk", |d: MStr, s: Str, _l: usize| -> MStr { libc::strcpy(d, s) });
    r!("stpcpy", |d: MStr, s: Str| -> MStr { sys::stpcpy(d, s) });
    r!("strncpy", |d: MStr, s: Str, n: usize| -> MStr { libc::strncpy(d, s, n) });
    r!("__strncpy_chk", |d: MStr, s: Str, n: usize, _l: usize| -> MStr { libc::strncpy(d, s, n) });
    r!("strcat", |d: MStr, s: Str| -> MStr { libc::strcat(d, s) });
    r!("__strcat_chk", |d: MStr, s: Str, _l: usize| -> MStr { libc::strcat(d, s) });
    r!("strncat", |d: MStr, s: Str, n: usize| -> MStr { libc::strncat(d, s, n) });
    r!("__strncat_chk", |d: MStr, s: Str, n: usize, _l: usize| -> MStr { libc::strncat(d, s, n) });
    r!("strlcpy", |d: MStr, s: Str, n: usize| -> usize {
        let l = libc::strlen(s);
        if n != 0 {
            let k = l.min(n - 1);
            std::ptr::copy_nonoverlapping(s, d, k);
            *d.add(k) = 0;
        }
        l
    });
    r!("strlcat", |d: MStr, s: Str, n: usize| -> usize {
        let (dl, sl) = (libc::strnlen(d, n), libc::strlen(s));
        if dl == n {
            return n + sl;
        }
        let k = sl.min(n - dl - 1);
        std::ptr::copy_nonoverlapping(s, d.add(dl), k);
        *d.add(dl + k) = 0;
        dl + sl
    });
    r!("strcmp", |x: Str, y: Str| -> i32 { libc::strcmp(x, y) });
    r!("strncmp", |x: Str, y: Str, n: usize| -> i32 { libc::strncmp(x, y, n) });
    r!("strcasecmp", |x: Str, y: Str| -> i32 { libc::strcasecmp(x, y) });
    r!("strncasecmp", |x: Str, y: Str, n: usize| -> i32 { libc::strncasecmp(x, y, n) });
    r!("strcoll", |x: Str, y: Str| -> i32 { libc::strcmp(x, y) });
    r!("strxfrm", |d: MStr, s: Str, n: usize| -> usize { sys::strxfrm(d, s, n) });
    for n in ["strchr", "index"] {
        r!(n, |s: Str, ch: i32| -> MStr { libc::strchr(s, ch) });
    }
    r!("__strchr_chk", |s: Str, ch: i32, _l: usize| -> MStr { libc::strchr(s, ch) });
    for n in ["strrchr", "rindex"] {
        r!(n, |s: Str, ch: i32| -> MStr { libc::strrchr(s, ch) });
    }
    r!("__strrchr_chk", |s: Str, ch: i32, _l: usize| -> MStr { libc::strrchr(s, ch) });
    r!("strstr", |h: Str, n: Str| -> MStr { libc::strstr(h, n) });
    r!("strcasestr", |h: Str, n: Str| -> MStr { sys::strcasestr(h, n) });
    r!("strpbrk", |s: Str, acc: Str| -> MStr { libc::strpbrk(s, acc) });
    r!("strspn", |s: Str, acc: Str| -> usize { libc::strspn(s, acc) });
    r!("strcspn", |s: Str, rej: Str| -> usize { libc::strcspn(s, rej) });
    r!("strtok", |s: MStr, d: Str| -> MStr { sys::strtok(s, d) });
    reg_raw("strtok_r", |c| unsafe {
        let mut a = ArgReader::new(c);
        let s: MStr = a.ptr();
        let d: Str = a.ptr();
        let save = a.next32();
        let mut sp: MStr = g2h(rd::<u32>(save));
        let r = sys::strtok_r(s, d, &mut sp);
        wr::<u32>(save, h2g(sp));
        c.set_ret32(h2g(r));
    });
    reg_raw("strsep", |c| unsafe {
        let mut a = ArgReader::new(c);
        let sp = a.next32();
        let d: Str = a.ptr();
        let cur: u32 = rd(sp);
        let mut s: MStr = g2h(cur);
        let r = if cur != 0 { sys::strsep(&mut s, d) } else { std::ptr::null_mut() };
        if cur != 0 {
            wr::<u32>(sp, h2g(s));
        }
        c.set_ret32(h2g(r));
    });
    r!("strdup", |s: Str| -> u32 { g_strdup(s) });
    r!("strndup", |s: Str, n: usize| -> u32 {
        let l = libc::strnlen(s, n);
        g_strdup_bytes(std::slice::from_raw_parts(s as *const u8, l))
    });
    r!("strerror", |e: i32| -> u32 { intern_cstr(libc::strerror(e)) });
    r!("strerror_r", |e: i32, buf: MStr, n: usize| -> i32 {
        if n != 0 {
            let s = crate::util::cbytes(libc::strerror(e));
            let k = s.len().min(n - 1);
            std::ptr::copy_nonoverlapping(s.as_ptr(), buf as *mut u8, k);
            *buf.add(k) = 0;
        }
        0
    });
    r!("strsignal", |s: i32| -> u32 { intern_cstr(sys::strsignal(s)) });

    reg_raw("strtol", |c| unsafe { with_strto_end(c, |c, a, s, e| c.set_ret32(sys::strtol(s, e, a.i32()) as u32)) });
    reg_raw("strtoul", |c| unsafe {
        with_strto_end(c, |c, a, s, e| {
            let v = sys::strtoull(s, e, a.i32());
            c.set_ret32(if v > 0xFFFF_FFFF && (v as i64) > 0 { 0xFFFF_FFFF } else { v as u32 });
        })
    });
    for n in ["strtoll", "strtoimax", "strtoq"] {
        reg_raw(n, |c| unsafe { with_strto_end(c, |c, a, s, e| c.set_ret64(sys::strtoll(s, e, a.i32()) as u64)) });
    }
    for n in ["strtoull", "strtoumax", "strtouq"] {
        reg_raw(n, |c| unsafe { with_strto_end(c, |c, a, s, e| c.set_ret64(sys::strtoull(s, e, a.i32()))) });
    }
    for n in ["strtod", "strtold"] {
        reg_raw(n, |c| unsafe { with_strto_end(c, |c, _a, s, e| c.set_ret64(sys::strtod(s, e).to_bits())) });
    }
    reg_raw("strtof", |c| unsafe { with_strto_end(c, |c, _a, s, e| c.set_ret32(sys::strtof(s, e).to_bits())) });
    r!("atoi", |s: Str| -> i32 { sys::atoi(s) });
    r!("atol", |s: Str| -> i32 { sys::atoi(s) });
    r!("atoll", |s: Str| -> i64 { sys::atoll(s) });
    r!("atof", |s: Str| -> f64 { sys::atof(s) });
}

fn reg_ctype() {
    macro_rules! ct {
        ($($name:ident),*) => {$( r!(stringify!($name), |ch: i32| -> i32 { libc::$name(ch) }); )*};
    }
    ct!(isalpha, isdigit, isalnum, isspace, isupper, islower, isxdigit, ispunct, isprint, isgraph, iscntrl, tolower, toupper);
    r!("isblank", |ch: i32| -> i32 { sys::isblank(ch) });
    reg("isascii", |ch: i32| -> i32 { ((ch & !0x7f) == 0) as i32 });
    reg("toascii", |ch: i32| -> i32 { ch & 0x7f });
    macro_rules! wct {
        ($($name:ident),*) => {$( r!(stringify!($name), |ch: u32| -> i32 { sys::$name(ch) as i32 }); )*};
    }
    wct!(iswalpha, iswdigit, iswalnum, iswspace, iswupper, iswlower, iswxdigit, iswpunct, iswprint, iswcntrl, towlower, towupper);
    r!("wcslen", |s: W| -> usize { sys::wcslen(s) });
    r!("wcscpy", |d: W, s: W| -> W { sys::wcscpy(d, s) });
    r!("wcsncpy", |d: W, s: W, n: usize| -> W { sys::wcsncpy(d, s, n) });
    r!("wcscat", |d: W, s: W| -> W { sys::wcscat(d, s) });
    r!("wcscmp", |x: W, y: W| -> i32 { sys::wcscmp(x, y) });
    r!("wcsncmp", |x: W, y: W, n: usize| -> i32 { sys::wcsncmp(x, y, n) });
    r!("wcscoll", |x: W, y: W| -> i32 { sys::wcscmp(x, y) });
    r!("wcschr", |s: W, ch: u32| -> W { sys::wcschr(s, ch) });
    r!("wcsrchr", |s: W, ch: u32| -> W { sys::wcsrchr(s, ch) });
    r!("wcsstr", |h: W, n: W| -> W { sys::wcsstr(h, n) });
    r!("wmemcpy", |d: W, s: W, n: usize| -> W { sys::wmemcpy(d, s, n) });
    r!("wmemmove", |d: W, s: W, n: usize| -> W { sys::wmemmove(d, s, n) });
    r!("wmemset", |d: W, ch: u32, n: usize| -> W { sys::wmemset(d, ch, n) });
    r!("wmemcmp", |x: W, y: W, n: usize| -> i32 { sys::wmemcmp(x, y, n) });
    r!("wmemchr", |s: W, ch: u32, n: usize| -> W { sys::wmemchr(s, ch, n) });
    r!("mbstowcs", |d: W, s: Str, n: usize| -> usize { sys::mbstowcs(d, s, n) });
    r!("wcstombs", |d: MStr, s: W, n: usize| -> usize { sys::wcstombs(d, s, n) });
    r!("mbtowc", |d: W, s: Str, n: usize| -> i32 { sys::mbtowc(d, s, n) });
    r!("wctomb", |d: MStr, ch: u32| -> i32 { sys::wctomb(d, ch) });
    r!("mblen", |s: Str, n: usize| -> i32 { sys::mblen(s, n) });
    r!("btowc", |ch: i32| -> u32 { sys::btowc(ch) });
    r!("wctob", |ch: u32| -> i32 { sys::wctob(ch) });
    r!("mbrtowc", |d: W, s: Str, n: usize, _st: u32| -> usize { sys::mbrtowc(d, s, n, std::ptr::null_mut()) });
    r!("wcrtomb", |d: MStr, ch: u32, _st: u32| -> usize { sys::wcrtomb(d, ch, std::ptr::null_mut()) });
    r!("mbrlen", |s: Str, n: usize, _st: u32| -> usize { sys::mbrlen(s, n, std::ptr::null_mut()) });
    reg("mbsinit", |_st: u32| -> i32 { 1 });
    r!("__ctype_get_mb_cur_max", | | -> usize { sys::__ctype_get_mb_cur_max() });
    reg("setlocale", |_c: i32, _l: u32| -> u32 { intern_str("C") });
    reg("newlocale", |_m: i32, _l: u32, _b: u32| -> u32 { intern_str("<locale>") });
    reg("duplocale", |l: u32| -> u32 { l });
    reg("freelocale", |_l: u32| {});
    reg("uselocale", |_l: u32| -> u32 { intern_str("<locale>") });
    reg_raw("localeconv", |c| unsafe {
        static G: AtomicU32 = AtomicU32::new(0);
        let mut g = G.load(Ordering::Relaxed);
        if g == 0 {
            // struct lconv: 10 char* followed by 14 chars.
            g = g_calloc(1, 64);
            let (dot, empty) = (intern_str("."), intern_str(""));
            for i in 0..10 {
                wr::<u32>(g + i * 4, empty);
            }
            wr::<u32>(g, dot);
            std::ptr::write_bytes(g2h::<u8>(g + 40), 0x7f, 14);
            G.store(g, Ordering::Relaxed);
        }
        c.set_ret32(g);
    });
}

fn reg_stdlib() {
    reg_raw("qsort", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (b, n, sz, cmp) = (a.next32(), a.next32(), a.next32(), a.next32());
        guest_qsort(c, b, n, sz, cmp);
    });
    reg_raw("bsearch", |c| {
        let mut a = ArgReader::new(c);
        let (key, base, n, size, cmp) = (a.next32(), a.next32(), a.next32(), a.next32(), a.next32());
        let (mut lo, mut hi, mut found) = (0u32, n, 0u32);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let elem = base + mid * size;
            let r = c.call(cmp, &[key, elem]) as i32;
            if r == 0 {
                found = elem;
                break;
            }
            if r < 0 {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        c.set_ret32(found);
    });
    reg("abs", |v: i32| -> i32 { v.wrapping_abs() });
    reg("labs", |v: i32| -> i32 { v.wrapping_abs() });
    reg("llabs", |v: i64| -> i64 { v.wrapping_abs() });
    r!("rand", | | -> i32 { libc::rand() & 0x7fff_ffff });
    r!("srand", |s: u32| { libc::srand(s) });
    r!("random", | | -> i32 { (sys::random() & 0x7fff_ffff) as i32 });
    r!("srandom", |s: u32| { sys::srandom(s) });
    r!("rand_r", |s: *mut u32| -> i32 { sys::rand_r(s) });
    r!("lrand48", | | -> i32 { sys::lrand48() as i32 });
    r!("mrand48", | | -> i32 { sys::mrand48() as i32 });
    r!("drand48", | | -> f64 { sys::drand48() });
    r!("srand48", |s: i32| { sys::srand48(s as c_long) });
    r!("arc4random", | | -> u32 { sys::random() as u32 ^ ((sys::random() as u32) << 16) });
    r!("arc4random_uniform", |n: u32| -> u32 { if n != 0 { sys::random() as u32 % n } else { 0 } });
    r!("getenv", |n: Str| -> u32 { intern_cstr(libc::getenv(n)) });
    r!("setenv", |n: Str, v: Str, o: i32| -> i32 { libc::setenv(n, v, o) });
    r!("unsetenv", |n: Str| -> i32 { libc::unsetenv(n) });
    reg("putenv", |_s: u32| -> i32 { 0 });
    reg("abort", || -> () {
        fatal!("guest called abort()");
    });
    r!("exit", |code: i32| {
        logw!("guest called exit({})", code);
        libc::exit(code);
    });
    for n in ["_exit", "_Exit"] {
        r!(n, |code: i32| { libc::_exit(code) });
    }
    reg("atexit", |_f: u32| -> i32 { 0 });
    reg("__cxa_atexit", |_f: u32, _a: u32, _d: u32| -> i32 { 0 });
    reg("__aeabi_atexit", |_a: u32, _f: u32, _d: u32| -> i32 { 0 });
    reg("__cxa_finalize", |_d: u32| {});
    reg("__cxa_thread_atexit_impl", |_f: u32, _a: u32, _d: u32| -> i32 { 0 });
    reg("__register_atfork", |_a: u32, _b: u32, _c: u32, _d: u32| -> i32 { 0 });
    reg("pthread_atfork", |_a: u32, _b: u32, _c: u32| -> i32 { 0 });
    reg("__stack_chk_fail", || -> () {
        fatal!("guest stack corruption detected (__stack_chk_fail)");
    });
    r!("__assert2", |file: Str, line: i32, func: Str, expr: Str| {
        fatal!("guest assertion failed: {}:{} {}: {}", cstr(file), line, if func.is_null() { "?".to_string() } else { cstr(func) }, cstr(expr));
    });
    r!("__assert", |file: Str, line: i32, expr: Str| {
        fatal!("guest assertion failed: {}:{}: {}", cstr(file), line, cstr(expr));
    });
    reg("__cxa_pure_virtual", || -> () {
        fatal!("guest: pure virtual function called");
    });
    reg("__errno", || -> u32 { Cpu::current().errno_addr });
    r!("realpath", |path: Str, out: MStr| -> u32 {
        let mut buf = [0 as c_char; libc::PATH_MAX as usize];
        if libc::realpath(path, buf.as_mut_ptr()).is_null() {
            return 0;
        }
        if !out.is_null() {
            libc::strcpy(out, buf.as_ptr());
            return h2g(out);
        }
        g_strdup(buf.as_ptr())
    });
    r!("mkstemp", |t: MStr| -> i32 { libc::mkstemp(t) });
    r!("mktemp", |t: MStr| -> MStr {
        let fd = libc::mkstemp(t);
        if fd >= 0 {
            libc::close(fd);
            libc::unlink(t);
        }
        t
    });
    reg("system", |_cmd: u32| -> i32 { -1 });
}

unsafe fn tm_conv(t: *const i32, out: *mut GTm, local: bool) -> u32 {
    let tt = *t as libc::time_t;
    let mut h: libc::tm = std::mem::zeroed();
    if local {
        libc::localtime_r(&tt, &mut h);
    } else {
        libc::gmtime_r(&tt, &mut h);
    }
    let dst = if out.is_null() { lazy_buf(&TM_BUF, std::mem::size_of::<GTm>() as u32) } else { h2g(out) };
    let zone = if h.tm_zone.is_null() { intern_str("UTC") } else { intern_cstr(h.tm_zone) };
    wr(dst, tm_to_guest(&h, zone));
    dst
}

fn reg_time() {
    reg_raw("time", |c| unsafe {
        let out = ArgReader::new(c).next32();
        let t = libc::time(std::ptr::null_mut());
        if out != 0 {
            wr::<i32>(out, t as i32);
        }
        c.set_ret32(t as u32);
    });
    r!("gettimeofday", |tv: *mut GTimeval, _tz: u32| -> i32 {
        probe_time_call(0, 0);
        if let Some(c) = Cpu::current_or_null() {
            frame_cap_on_clock(c);
        }
        let mut h: libc::timeval = std::mem::zeroed();
        libc::gettimeofday(&mut h, std::ptr::null_mut());
        if !tv.is_null() {
            tv.write_unaligned(GTimeval { tv_sec: h.tv_sec as i32, tv_usec: h.tv_usec as i32 });
        }
        0
    });
    r!("clock_gettime", |clk: i32, ts: *mut GTimespec| -> i32 {
        probe_time_call(1, 0);
        let mut h: libc::timespec = std::mem::zeroed();
        let r = libc::clock_gettime(clk, &mut h);
        if r == 0 && !ts.is_null() {
            ts.write_unaligned(ts_to_guest(&h));
        }
        r
    });
    r!("clock_getres", |clk: i32, ts: *mut GTimespec| -> i32 {
        let mut h: libc::timespec = std::mem::zeroed();
        let r = libc::clock_getres(clk, &mut h);
        if r == 0 && !ts.is_null() {
            ts.write_unaligned(ts_to_guest(&h));
        }
        r
    });
    r!("clock", | | -> u32 { sys::clock() as u32 });
    r!("nanosleep", |req: *const GTimespec, rem: *mut GTimespec| -> i32 {
        let _blocking = BlockingCall::new();
        let mut h = ts_from_guest(&req.read_unaligned());
        let mut hr: libc::timespec = std::mem::zeroed();
        probe_time_call(2, h.tv_sec as u64 * 1_000_000 + h.tv_nsec as u64 / 1000);
        let mode = limiter_mode_for_this_thread();
        if mode != 0 {
            if mode == 2 {
                if !rem.is_null() {
                    rem.write_unaligned(ts_to_guest(&hr));
                }
                return 0;
            }
            let ns = (h.tv_sec as i64 * 1_000_000_000 + h.tv_nsec as i64) / 2;
            h.tv_sec = (ns / 1_000_000_000) as _;
            h.tv_nsec = (ns % 1_000_000_000) as _;
        }
        let r = libc::nanosleep(&h, &mut hr);
        if !rem.is_null() {
            rem.write_unaligned(ts_to_guest(&hr));
        }
        r
    });
    r!("clock_nanosleep", |clk: i32, flags: i32, req: *const GTimespec, rem: *mut GTimespec| -> i32 {
        let h = ts_from_guest(&req.read_unaligned());
        let mut hr: libc::timespec = std::mem::zeroed();
        let r = sys::clock_nanosleep(clk, flags, &h, &mut hr);
        if !rem.is_null() {
            rem.write_unaligned(ts_to_guest(&hr));
        }
        r
    });
    r!("waitpid", |pid: i32, status: *mut i32, options: i32| -> i32 {
        let _blocking = BlockingCall::new();
        libc::waitpid(pid, status, options)
    });
    r!("usleep", |us: u32| -> i32 {
        let _blocking = BlockingCall::new();
        let mut us = us;
        probe_time_call(2, us as u64);
        if let Some(c) = Cpu::current_or_null() {
            frame_cap_on_sleep(c, &mut us);
        }
        let mode = limiter_mode_for_this_thread();
        if mode != 0 {
            if mode == 2 {
                return 0;
            }
            us /= 2;
        }
        libc::usleep(us)
    });
    r!("sleep", |s: u32| -> u32 {
        let _blocking = BlockingCall::new();
        libc::sleep(s)
    });
    r!("localtime", |t: *const i32| -> u32 { tm_conv(t, std::ptr::null_mut(), true) });
    r!("gmtime", |t: *const i32| -> u32 { tm_conv(t, std::ptr::null_mut(), false) });
    r!("localtime_r", |t: *const i32, o: *mut GTm| -> u32 { tm_conv(t, o, true) });
    r!("gmtime_r", |t: *const i32, o: *mut GTm| -> u32 { tm_conv(t, o, false) });
    r!("mktime", |g: *mut GTm| -> i32 {
        let gv = g.read_unaligned();
        let mut h = tm_from_guest(&gv);
        let r = libc::mktime(&mut h);
        g.write_unaligned(tm_to_guest(&h, gv.tm_zone));
        r as i32
    });
    r!("timegm", |g: *mut GTm| -> i32 {
        let mut h = tm_from_guest(&g.read_unaligned());
        sys::timegm(&mut h) as i32
    });
    r!("strftime", |buf: MStr, n: usize, fmt: Str, g: *const GTm| -> usize {
        let h = tm_from_guest(&g.read_unaligned());
        libc::strftime(buf, n, fmt, &h)
    });
    r!("asctime", |g: *const GTm| -> u32 {
        let h = tm_from_guest(&g.read_unaligned());
        let b = lazy_buf(&CTIME_BUF, 64);
        sys::asctime_r(&h, g2h(b));
        b
    });
    r!("ctime", |t: *const i32| -> u32 {
        let tt = *t as libc::time_t;
        let b = lazy_buf(&CTIME_BUF, 64);
        sys::ctime_r(&tt, g2h(b));
        b
    });
    reg("difftime", |x: i32, y: i32| -> f64 { x as f64 - y as f64 });
    r!("tzset", | | { sys::tzset() });
}

fn reg_files() {
    for n in ["open", "open64"] {
        reg_raw(n, |c| unsafe {
            let mut a = ArgReader::new(c);
            let (p, fl, mode) = (a.ptr::<c_char>(), a.i32(), a.i32());
            c.set_ret32(guest_open(p, fl, mode) as u32);
        });
    }
    reg_raw("__open_2", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (p, fl) = (a.ptr::<c_char>(), a.i32());
        c.set_ret32(guest_open(p, fl, 0) as u32);
    });
    reg_raw("openat", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (d, p, fl, mode) = (a.i32(), a.ptr::<c_char>(), a.i32(), a.i32());
        let v = guest_proc_open(p);
        c.set_ret32(if v != -2 { v } else { libc::openat(d, p, translate_open_flags(fl), mode as libc::c_uint) } as u32);
    });
    r!("creat", |p: Str, mode: i32| -> i32 { libc::creat(p, mode as libc::mode_t) });
    r!("close", |fd: i32| -> i32 { libc::close(fd) });
    r!("read", |fd: i32, b: P, n: usize| -> isize {
        let _blocking = BlockingCall::new();
        libc::read(fd, b, n)
    });
    r!("__read_chk", |fd: i32, b: P, n: usize, _l: usize| -> isize { libc::read(fd, b, n) });
    r!("write", |fd: i32, b: CP, n: usize| -> isize { libc::write(fd, b, n) });
    r!("pread", |fd: i32, b: P, n: usize, off: i32| -> isize { libc::pread(fd, b, n, off as libc::off_t) });
    r!("pwrite", |fd: i32, b: CP, n: usize, off: i32| -> isize { libc::pwrite(fd, b, n, off as libc::off_t) });
    r!("pread64", |fd: i32, b: P, n: usize, off: i64| -> isize { libc::pread(fd, b, n, off as libc::off_t) });
    r!("pwrite64", |fd: i32, b: CP, n: usize, off: i64| -> isize { libc::pwrite(fd, b, n, off as libc::off_t) });
    r!("lseek", |fd: i32, off: i32, wh: i32| -> i32 { libc::lseek(fd, off as libc::off_t, wh) as i32 });
    r!("lseek64", |fd: i32, off: i64, wh: i32| -> i64 { libc::lseek(fd, off as libc::off_t, wh) as i64 });
    r!("fsync", |fd: i32| -> i32 { libc::fsync(fd) });
    r!("fdatasync", |fd: i32| -> i32 { libc::fdatasync(fd) });
    r!("ftruncate", |fd: i32, len: i32| -> i32 { libc::ftruncate(fd, len as libc::off_t) });
    r!("ftruncate64", |fd: i32, len: i64| -> i32 { libc::ftruncate(fd, len as libc::off_t) });
    r!("truncate", |p: Str, len: i32| -> i32 { libc::truncate(p, len as libc::off_t) });
    r!("access", |p: Str, m: i32| -> i32 { libc::access(p, m) });
    r!("utime", |p: Str, times: *const i32| -> i32 {
        if times.is_null() {
            return libc::utimensat(libc::AT_FDCWD, p, std::ptr::null(), 0);
        }
        // guest struct utimbuf { actime, modtime }
        let mut ts: [libc::timespec; 2] = std::mem::zeroed();
        ts[0].tv_sec = *times as _;
        ts[1].tv_sec = *times.add(1) as _;
        libc::utimensat(libc::AT_FDCWD, p, ts.as_ptr(), 0)
    });
    r!("pathconf", |p: Str, name: i32| -> i32 { libc::pathconf(p, name) as i32 });
    r!("fpathconf", |fd: i32, name: i32| -> i32 { libc::fpathconf(fd, name) as i32 });
    r!("fnmatch", |pattern: Str, s: Str, flags: i32| -> i32 { sys::fnmatch(pattern, s, flags) });
    // The result points into the argument (GNU behaviour): host basename() may return host-only storage.
    for n in ["basename", "__gnu_basename"] {
        r!(n, |p: Str| -> Str {
            if p.is_null() {
                return p;
            }
            let slash = libc::strrchr(p, b'/' as c_int);
            if slash.is_null() { p } else { slash.add(1) as Str }
        });
    }
    r!("unlink", |p: Str| -> i32 { libc::unlink(p) });
    r!("rmdir", |p: Str| -> i32 { libc::rmdir(p) });
    r!("mkdir", |p: Str, m: i32| -> i32 { libc::mkdir(p, m as libc::mode_t) });
    r!("chdir", |p: Str| -> i32 { libc::chdir(p) });
    r!("chmod", |p: Str, m: i32| -> i32 { libc::chmod(p, m as libc::mode_t) });
    r!("fchmod", |fd: i32, m: i32| -> i32 { libc::fchmod(fd, m as libc::mode_t) });
    r!("rename", |x: Str, y: Str| -> i32 { libc::rename(x, y) });
    r!("remove", |p: Str| -> i32 { libc::remove(p) });
    r!("link", |x: Str, y: Str| -> i32 { libc::link(x, y) });
    r!("symlink", |x: Str, y: Str| -> i32 { libc::symlink(x, y) });
    r!("readlink", |p: Str, b: MStr, n: usize| -> isize { libc::readlink(p, b, n) });
    r!("getcwd", |b: MStr, n: usize| -> MStr { libc::getcwd(b, n) });
    r!("dup", |fd: i32| -> i32 { libc::dup(fd) });
    r!("dup2", |x: i32, y: i32| -> i32 { libc::dup2(x, y) });
    r!("pipe", |fds: *mut i32| -> i32 { libc::pipe(fds) });
    r!("isatty", |fd: i32| -> i32 { libc::isatty(fd) });
    r!("umask", |m: i32| -> i32 { libc::umask(m as libc::mode_t) as i32 });
    r!("getpid", | | -> i32 { libc::getpid() });
    r!("getppid", | | -> i32 { libc::getppid() });
    reg("gettid", || -> i32 { crate::util::gettid() });
    r!("getuid", | | -> i32 { libc::getuid() as i32 });
    r!("geteuid", | | -> i32 { libc::geteuid() as i32 });
    r!("getgid", | | -> i32 { libc::getgid() as i32 });
    r!("getegid", | | -> i32 { libc::getegid() as i32 });
    reg("getpagesize", || -> i32 { 4096 });
    r!("sysconf", |name: i32| -> i32 {
        // bionic and the host C library use different _SC_ numbers; handle the common ones.
        match name {
            0x27 | 0x28 => 4096,                                         // _SC_PAGESIZE / _SC_PAGE_SIZE
            0x60 => libc::sysconf(libc::_SC_NPROCESSORS_CONF) as i32,    // _SC_NPROCESSORS_CONF
            0x61 => libc::sysconf(libc::_SC_NPROCESSORS_ONLN) as i32,    // _SC_NPROCESSORS_ONLN
            0x62 => libc::sysconf(libc::_SC_PHYS_PAGES).min(0x7fff_ffff) as i32,
            0x06 => 100,                                                 // _SC_CLK_TCK
            0x00 => 131072,                                              // _SC_ARG_MAX
            0x0a => 1024,                                                // _SC_OPEN_MAX
            _ => -1,
        }
    });
    r!("sched_yield", | | -> i32 {
        probe_time_call(3, 0);
        libc::sched_yield()
    });
    reg("sched_getcpu", || -> i32 { 0 });
    r!("sched_get_priority_max", |p: i32| -> i32 { libc::sched_get_priority_max(p) });
    r!("sched_get_priority_min", |p: i32| -> i32 { libc::sched_get_priority_min(p) });
    reg("sched_setscheduler", |_p: i32, _pol: i32, _param: u32| -> i32 { 0 });
    reg("sched_setaffinity", |_p: i32, _n: u32, _m: u32| -> i32 { 0 });
    r!("setpriority", |which: i32, who: i32, prio: i32| -> i32 {
        // Thread priorities matter on phones: the scheduler keeps busy, high-priority threads on fast cores.
        // Not permitted at this level: behave as before (succeed silently).
        libc::setpriority(which as _, who as libc::id_t, prio);
        0
    });
    reg("getpriority", |_w: i32, _who: i32| -> i32 { 0 });
    r!("nice", |inc: i32| -> i32 {
        set_errno(0);
        let r = sys::nice(inc);
        if r == -1 && errno() != 0 { 0 } else { r }
    });
    reg("prctl", |_o: i32, _a: u32, _b: u32, _c: u32, _d: u32| -> i32 { 0 });
    r!("getrlimit", |_r: i32, out: *mut u32| -> i32 {
        *out = 0x7fff_ffff;
        *out.add(1) = 0x7fff_ffff;
        0
    });
    reg("setrlimit", |_r: i32, _l: u32| -> i32 { 0 });
    reg_raw("fcntl", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (fd, cmd, arg) = (a.i32(), a.i32(), a.next32());
        c.set_ret32(libc::fcntl(fd, cmd, arg as c_long) as u32);
    });
    reg_raw("syscall", |c| {
        let mut a = ArgReader::new(c);
        let nr = a.next32();
        let mut args = [0u32; 6];
        for x in args.iter_mut() {
            *x = a.next32();
        }
        let mut r = crate::thunks::misc::guest_syscall_args(c, nr, &args);
        if (r as i32) < 0 && (r as i32) >= -4095 {
            set_errno(-(r as i32));
            r = u32::MAX;
        }
        c.set_ret32(r);
    });

    for (n, follow) in [("stat", true), ("lstat", false), ("stat64", true), ("lstat64", false)] {
        r!(n, |p: Str, out: *mut GStat| -> i32 {
            let mut s: libc::stat = std::mem::zeroed();
            let r = if follow { libc::stat(p, &mut s) } else { libc::lstat(p, &mut s) };
            if r == 0 {
                stat_to_guest(out, &s);
            }
            r
        });
    }
    for n in ["fstat", "fstat64"] {
        r!(n, |fd: i32, out: *mut GStat| -> i32 {
            let mut s: libc::stat = std::mem::zeroed();
            let r = libc::fstat(fd, &mut s);
            if r == 0 {
                stat_to_guest(out, &s);
            }
            r
        });
    }
    r!("fstatat", |d: i32, p: Str, out: *mut GStat, fl: i32| -> i32 {
        let mut s: libc::stat = std::mem::zeroed();
        let r = libc::fstatat(d, p, &mut s, fl);
        if r == 0 {
            stat_to_guest(out, &s);
        }
        r
    });

    // directories (struct dirent has the same layout in 32-bit bionic and on 64-bit hosts)
    const DIRENT: usize = std::mem::size_of::<libc::dirent>();
    r!("opendir", |p: Str| -> u32 {
        let d = libc::opendir(p);
        if d.is_null() {
            return 0;
        }
        let h = g_calloc(1, DIRENT as u32 + 16);
        lock(&DIRS).get_or_insert_with(HashMap::new).insert(h, GuestDir { dir: d as usize, entry: h });
        h
    });
    r!("readdir", |h: u32| -> u32 {
        let Some((dir, entry)) = find_dir(h) else { return 0 };
        let e = libc::readdir(dir);
        if e.is_null() {
            return 0;
        }
        std::ptr::copy_nonoverlapping(e as *const u8, g2h::<u8>(entry), DIRENT);
        entry
    });
    r!("readdir_r", |h: u32, entry: *mut u8, result: *mut u32| -> i32 {
        let e = match find_dir(h) {
            Some((dir, _)) => libc::readdir(dir),
            None => std::ptr::null_mut(),
        };
        if e.is_null() {
            *result = 0;
            return 0;
        }
        std::ptr::copy_nonoverlapping(e as *const u8, entry, DIRENT);
        *result = h2g(entry);
        0
    });
    r!("rewinddir", |h: u32| {
        if let Some((dir, _)) = find_dir(h) {
            libc::rewinddir(dir);
        }
    });
    r!("dirfd", |h: u32| -> i32 {
        match find_dir(h) {
            Some((dir, _)) => libc::dirfd(dir),
            None => -1,
        }
    });
    r!("closedir", |h: u32| -> i32 {
        let Some(gd) = lock(&DIRS).as_mut().and_then(|m| m.remove(&h)) else { return -1 };
        g_free(h);
        libc::closedir(gd.dir as *mut libc::DIR)
    });
}

fn reg_net_and_signals() {
    type Sa = *mut libc::sockaddr;
    type Sl = *mut libc::socklen_t;
    r!("socket", |d: i32, t: i32, p: i32| -> i32 { libc::socket(d, t, p) });
    r!("connect", |fd: i32, sa: Sa, l: u32| -> i32 { libc::connect(fd, sa, l) });
    r!("bind", |fd: i32, sa: Sa, l: u32| -> i32 { libc::bind(fd, sa, l) });
    r!("listen", |fd: i32, n: i32| -> i32 { libc::listen(fd, n) });
    r!("accept", |fd: i32, sa: Sa, l: Sl| -> i32 {
        let _blocking = BlockingCall::new();
        libc::accept(fd, sa, l)
    });
    r!("send", |fd: i32, b: CP, n: usize, f: i32| -> isize { libc::send(fd, b, n, f) });
    r!("recv", |fd: i32, b: P, n: usize, f: i32| -> isize {
        let _blocking = BlockingCall::new();
        libc::recv(fd, b, n, f)
    });
    r!("sendto", |fd: i32, b: CP, n: usize, f: i32, sa: Sa, l: u32| -> isize { libc::sendto(fd, b, n, f, sa, l) });
    r!("recvfrom", |fd: i32, b: P, n: usize, f: i32, sa: Sa, l: Sl| -> isize {
        let _blocking = BlockingCall::new();
        libc::recvfrom(fd, b, n, f, sa, l)
    });
    r!("shutdown", |fd: i32, how: i32| -> i32 { libc::shutdown(fd, how) });
    r!("setsockopt", |fd: i32, lvl: i32, opt: i32, v: CP, l: u32| -> i32 { libc::setsockopt(fd, lvl, opt, v, l) });
    r!("getsockopt", |fd: i32, lvl: i32, opt: i32, v: P, l: Sl| -> i32 { libc::getsockopt(fd, lvl, opt, v, l) });
    r!("getsockname", |fd: i32, sa: Sa, l: Sl| -> i32 { libc::getsockname(fd, sa, l) });
    r!("getpeername", |fd: i32, sa: Sa, l: Sl| -> i32 { libc::getpeername(fd, sa, l) });
    r!("poll", |fds: *mut libc::pollfd, n: u32, timeout: i32| -> i32 {
        let _blocking = BlockingCall::new();
        libc::poll(fds, n as libc::nfds_t, timeout)
    });
    r!("select", |n: i32, rd: *mut libc::fd_set, wr: *mut libc::fd_set, ex: *mut libc::fd_set, tv: *mut GTimeval| -> i32 {
        let _blocking = BlockingCall::new();
        let mut h: libc::timeval = std::mem::zeroed();
        let mut hp: *mut libc::timeval = std::ptr::null_mut();
        if !tv.is_null() {
            let g = tv.read_unaligned();
            h.tv_sec = g.tv_sec as _;
            h.tv_usec = g.tv_usec as _;
            hp = &mut h;
        }
        libc::select(n, rd, wr, ex, hp)
    });
    r!("inet_addr", |s: Str| -> u32 {
        let text = cstr(s);
        let parts: Vec<Option<u32>> = text.trim().splitn(4, '.').map(|p| p.trim_end_matches(|ch: char| !ch.is_ascii_digit()).parse::<u32>().ok()).collect();
        if parts.len() != 4 || parts.iter().any(|p| p.is_none()) {
            return 0xFFFF_FFFF;
        }
        let v: Vec<u32> = parts.into_iter().flatten().collect();
        v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24)
    });
    reg("htonl", |v: u32| -> u32 { v.swap_bytes() });
    reg("ntohl", |v: u32| -> u32 { v.swap_bytes() });
    reg("htons", |v: u32| -> u32 { (v as u16).swap_bytes() as u32 });
    reg("ntohs", |v: u32| -> u32 { (v as u16).swap_bytes() as u32 });

    // signal sets and masks
    // (handlers, raise, kill and sigsuspend are in signals.rs; masks are not emulated)
    reg("sigprocmask", |_h: i32, _s: u32, _o: u32| -> i32 { 0 });
    reg("pthread_sigmask", |_h: i32, _s: u32, _o: u32| -> i32 { 0 });
    r!("sigemptyset", |s: *mut u32| -> i32 {
        *s = 0;
        0
    });
    r!("sigfillset", |s: *mut u32| -> i32 {
        *s = 0xFFFF_FFFF;
        0
    });
    r!("sigaddset", |s: *mut u32, n: i32| -> i32 {
        *s |= 1 << ((n - 1) & 31);
        0
    });
    r!("sigdelset", |s: *mut u32, n: i32| -> i32 {
        *s &= !(1 << ((n - 1) & 31));
        0
    });
    r!("sigismember", |s: *const u32, n: i32| -> i32 { ((*s >> ((n - 1) & 31)) & 1) as i32 });
    reg("sigaltstack", |_s: u32, _o: u32| -> i32 { 0 });
    reg("alarm", |_s: u32| -> u32 { 0 });

    // system properties
    r!("__system_property_get", |name: Str, value: MStr| -> i32 {
        #[cfg(target_os = "android")]
        {
            extern "C" {
                fn __system_property_get(name: *const c_char, value: *mut c_char) -> c_int;
            }
            __system_property_get(name, value)
        }
        #[cfg(not(target_os = "android"))]
        {
            let _ = name;
            *value = 0;
            0
        }
    });
    reg("__system_property_find", |_n: u32| -> u32 { 0 });
}
