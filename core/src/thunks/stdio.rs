//! stdio: FILE handles, printf/scanf families with guest varargs.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::collections::HashMap;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use crate::mem::*;
use crate::proc::guest_proc_open;
use crate::thunk::{reg, reg_raw, thunk_register_data, ArgReader, VaSource};
use crate::util::{cbytes, lock};

type File = *mut libc::FILE;
type Str = *const c_char;

extern "C" {
    static mut stdin: File;
    static mut stdout: File;
    static mut stderr: File;
}
fn std_stream(i: u32) -> File {
    unsafe {
        match i {
            0 => stdin,
            1 => stdout,
            _ => stderr,
        }
    }
}

fn spec_c(spec: &[u8]) -> CString {
    CString::new(spec.to_vec()).unwrap_or_default()
}

// Formats one value with the host's snprintf.
macro_rules! host_fmt {
    ($out:expr, $spec:expr, $($arg:expr),*) => {{
        let spec = spec_c($spec);
        let mut buf = [0u8; 512];
        let n = libc::snprintf(buf.as_mut_ptr() as *mut c_char, buf.len(), spec.as_ptr(), $($arg),*);
        if n >= buf.len() as c_int {
            let mut big = vec![0u8; n as usize + 1];
            libc::snprintf(big.as_mut_ptr() as *mut c_char, big.len(), spec.as_ptr(), $($arg),*);
            $out.extend_from_slice(&big[..n as usize]);
        } else if n > 0 {
            $out.extend_from_slice(&buf[..n as usize]);
        }
    }};
}

/// printf-style formatting with guest arguments. Guest sizes: int/long/size_t/pointer = 32 bit,
/// long long = 64 bit, double = 64 bit (8-byte aligned).
pub unsafe fn guest_format(fmt: *const c_char, va: &mut VaSource) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::new();
    if fmt.is_null() {
        return out;
    }
    let f = cbytes(fmt);
    let at = |i: usize| -> u8 { f.get(i).copied().unwrap_or(0) };
    let mut p = 0usize;
    while p < f.len() {
        if f[p] != b'%' {
            let q = f[p..].iter().position(|&b| b == b'%').map(|i| p + i).unwrap_or(f.len());
            out.extend_from_slice(&f[p..q]);
            p = q;
            continue;
        }
        p += 1;
        if at(p) == b'%' {
            out.push(b'%');
            p += 1;
            continue;
        }
        let mut spec: Vec<u8> = vec![b'%'];
        while at(p) != 0 && b"-+ #0'".contains(&at(p)) {
            spec.push(at(p));
            p += 1;
        }
        if at(p) == b'*' {
            spec.extend_from_slice((va.next32() as i32).to_string().as_bytes());
            p += 1;
        } else {
            while at(p).is_ascii_digit() {
                spec.push(at(p));
                p += 1;
            }
        }
        if at(p) == b'.' {
            spec.push(b'.');
            p += 1;
            if at(p) == b'*' {
                let prec = va.next32() as i32;
                if prec >= 0 {
                    spec.extend_from_slice(prec.to_string().as_bytes());
                } else {
                    spec.pop();
                }
                p += 1;
            } else {
                while at(p).is_ascii_digit() {
                    spec.push(at(p));
                    p += 1;
                }
            }
        }
        let (mut is64, mut is_wide) = (false, false);
        loop {
            match at(p) {
                b'l' => {
                    if at(p + 1) == b'l' {
                        is64 = true;
                        p += 1;
                    } else {
                        is_wide = true;
                    }
                }
                b'q' | b'j' => is64 = true,
                b'h' | b'z' | b't' | b'L' => {}
                _ => break,
            }
            p += 1;
        }
        let conv = at(p);
        if conv == 0 {
            break;
        }
        p += 1;
        match conv {
            b'd' | b'i' => {
                let v: i64 = if is64 { va.next64() as i64 } else { va.next32() as i32 as i64 };
                spec.extend_from_slice(b"lld");
                host_fmt!(out, &spec, v as libc::c_longlong);
            }
            b'u' | b'x' | b'X' | b'o' => {
                let v: u64 = if is64 { va.next64() } else { va.next32() as u64 };
                spec.extend_from_slice(b"ll");
                spec.push(conv);
                host_fmt!(out, &spec, v as libc::c_ulonglong);
            }
            b'c' => {
                spec.push(b'c');
                let ch = va.next32() as c_int;
                if ch == 0 && spec.len() == 2 {
                    out.push(0);
                } else {
                    host_fmt!(out, &spec, ch);
                }
            }
            b'p' => out.extend_from_slice(format!("0x{:x}", va.next32()).as_bytes()),
            b's' => {
                let gp = va.next32();
                let mut tmp: Vec<u8> = Vec::new();
                let s: *const c_char = if gp != 0 && is_wide {
                    let mut w = gp;
                    loop {
                        let ch: u32 = rd(w);
                        if ch == 0 {
                            break;
                        }
                        tmp.push(if ch < 128 { ch as u8 } else { b'?' });
                        w += 4;
                    }
                    tmp.push(0);
                    tmp.as_ptr() as *const c_char
                } else if gp != 0 {
                    g2h::<c_char>(gp)
                } else {
                    c"(null)".as_ptr()
                };
                if spec.len() == 1 {
                    out.extend_from_slice(cbytes(s)); // plain %s: no host formatting needed
                } else {
                    spec.push(b's');
                    let cs = spec_c(&spec);
                    let n = libc::snprintf(std::ptr::null_mut(), 0, cs.as_ptr(), s);
                    if n > 0 {
                        let old = out.len();
                        out.resize(old + n as usize + 1, 0);
                        libc::snprintf(out.as_mut_ptr().add(old) as *mut c_char, n as usize + 1, cs.as_ptr(), s);
                        out.truncate(old + n as usize);
                    }
                }
            }
            b'f' | b'F' | b'e' | b'E' | b'g' | b'G' | b'a' | b'A' => {
                let d = f64::from_bits(va.next64());
                spec.push(conv);
                host_fmt!(out, &spec, d);
            }
            b'n' => {
                let gp = va.next32();
                if gp != 0 {
                    wr::<i32>(gp, out.len() as i32);
                }
            }
            _ => {
                out.extend_from_slice(&spec);
                out.push(conv);
            }
        }
    }
    out
}

// ---- FILE* mapping: the guest sees the address of a dummy block ----
const GUEST_FILE_SIZE: u32 = 84; // sizeof(FILE) in 32-bit bionic
static FILES: Mutex<Option<HashMap<u32, usize>>> = Mutex::new(None);
static G_SF: AtomicU32 = AtomicU32::new(0);

/// Host FILE of a guest FILE* (NULL if unknown).
pub fn guest_file_get(g: u32) -> File {
    if g == 0 {
        return std::ptr::null_mut();
    }
    let sf = G_SF.load(Ordering::Relaxed);
    if sf != 0 && g >= sf && g < sf + 3 * GUEST_FILE_SIZE {
        return std_stream((g - sf) / GUEST_FILE_SIZE);
    }
    match lock(&FILES).as_ref().and_then(|m| m.get(&g).copied()) {
        Some(f) => f as File,
        None => {
            logw!("guest used an unknown FILE* 0x{:08x}", g);
            std::ptr::null_mut()
        }
    }
}
use guest_file_get as hf;

/// Guest FILE* for a host FILE (0 for NULL).
pub fn guest_file_new(f: File) -> u32 {
    if f.is_null() {
        return 0;
    }
    let g = g_calloc(1, GUEST_FILE_SIZE);
    // Old bionic headers read the descriptor straight from the structure (fileno() as a macro):
    // struct __sFILE { unsigned char* _p; int _r; int _w; short _flags; short _file; ... }.
    unsafe {
        wr::<i16>(g + 12, 0x10); // __SRW
        wr::<i16>(g + 14, libc::fileno(f) as i16);
    }
    lock(&FILES).get_or_insert_with(HashMap::new).insert(g, f as usize);
    g
}
use guest_file_new as new_file;

fn drop_file(g: u32) -> File {
    lock(&FILES).as_mut().and_then(|m| m.remove(&g)).unwrap_or(0) as File
}

unsafe fn emit(f: File, s: &[u8]) {
    #[cfg(target_os = "android")]
    if f == stdout || f == stderr {
        crate::log::tagged(crate::log::LOG_INFO, if f == stdout { "guest-stdout" } else { "guest-stderr" }, &String::from_utf8_lossy(s));
        return;
    }
    if !f.is_null() {
        libc::fwrite(s.as_ptr() as *const c_void, 1, s.len(), f);
    }
}

unsafe fn store(dst: *mut u8, cap: usize, bounded: bool, s: &[u8]) -> u32 {
    if !bounded {
        std::ptr::copy_nonoverlapping(s.as_ptr(), dst, s.len());
        *dst.add(s.len()) = 0;
    } else if cap != 0 {
        let n = s.len().min(cap - 1);
        std::ptr::copy_nonoverlapping(s.as_ptr(), dst, n);
        *dst.add(n) = 0;
    }
    s.len() as u32
}

// Rewrites a scanf format so that every conversion stores a value of the size
// the guest expects, then lets the host scanf write straight into guest memory.
fn scanf_format(f: &[u8]) -> (Vec<u8>, usize) {
    let mut out: Vec<u8> = Vec::new();
    let mut nargs = 0;
    let at = |i: usize| -> u8 { f.get(i).copied().unwrap_or(0) };
    let mut p = 0;
    while p < f.len() {
        if f[p] != b'%' {
            out.push(f[p]);
            p += 1;
            continue;
        }
        out.push(b'%');
        p += 1;
        if at(p) == b'%' {
            out.push(b'%');
            p += 1;
            continue;
        }
        let mut suppress = false;
        if at(p) == b'*' {
            suppress = true;
            out.push(b'*');
            p += 1;
        }
        while at(p).is_ascii_digit() {
            out.push(at(p));
            p += 1;
        }
        let (mut is64, mut is_short, mut is_char, mut is_l) = (false, false, false, false);
        loop {
            match at(p) {
                b'l' => {
                    if at(p + 1) == b'l' {
                        is64 = true;
                        p += 1;
                    } else {
                        is_l = true;
                    }
                }
                b'q' | b'j' => is64 = true,
                b'h' => {
                    if at(p + 1) == b'h' {
                        is_char = true;
                        p += 1;
                    } else {
                        is_short = true;
                    }
                }
                b'z' | b't' => {}
                b'L' => is_l = true,
                _ => break,
            }
            p += 1;
        }
        let conv = at(p);
        if conv == 0 {
            break;
        }
        p += 1;
        match conv {
            b'd' | b'i' | b'u' | b'x' | b'X' | b'o' | b'n' => {
                if is64 {
                    out.extend_from_slice(b"ll");
                } else if is_char {
                    out.extend_from_slice(b"hh");
                } else if is_short {
                    out.push(b'h');
                }
                out.push(conv);
            }
            b'f' | b'e' | b'g' | b'E' | b'G' | b'a' => {
                if is_l {
                    out.push(b'l');
                }
                out.push(conv);
            }
            b'p' => out.push(b'x'),
            b'[' => {
                out.push(b'[');
                if at(p) == b'^' {
                    out.push(b'^');
                    p += 1;
                }
                if at(p) == b']' {
                    out.push(b']');
                    p += 1;
                }
                while at(p) != 0 && at(p) != b']' {
                    out.push(at(p));
                    p += 1;
                }
                if at(p) == b']' {
                    out.push(b']');
                    p += 1;
                }
            }
            _ => out.push(conv), // s, c
        }
        if !suppress {
            nargs += 1;
        }
    }
    (out, nargs)
}

const MAX_SCAN: usize = 20;

enum ScanSrc {
    Str(Str),
    File(File),
}

unsafe fn do_scan(src: ScanSrc, fmt: Str, va: &mut VaSource) -> i32 {
    let (hfmt, nargs) = scanf_format(cbytes(fmt));
    if nargs > MAX_SCAN {
        logw!("scanf with {} conversions is not supported", nargs);
        return -1;
    }
    let mut p = [std::ptr::null_mut::<c_void>(); MAX_SCAN];
    for slot in p.iter_mut().take(nargs) {
        *slot = g2h(va.next32());
    }
    let h = spec_c(&hfmt);
    match src {
        ScanSrc::Str(s) => libc::sscanf(
            s, h.as_ptr(), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19],
        ),
        ScanSrc::File(f) => libc::fscanf(
            f, h.as_ptr(), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19],
        ),
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

const EOF: i32 = -1;

pub fn register_stdio_thunks() {
    // Standard streams: old NDKs reference the __sF array, newer ones the stdin/stdout/stderr pointers.
    let sf = g_calloc(3, GUEST_FILE_SIZE);
    G_SF.store(sf, Ordering::Relaxed);
    unsafe {
        for i in 0..3u32 {
            // descriptor field of stdin, stdout and stderr (see guest_file_new)
            wr::<i16>(sf + i * GUEST_FILE_SIZE + 12, if i == 0 { 0x4 } else { 0x8 }); // __SRD / __SWR
            wr::<i16>(sf + i * GUEST_FILE_SIZE + 14, i as i16);
        }
        thunk_register_data("__sF", sf);
        for (i, name) in ["stdin", "stdout", "stderr"].iter().enumerate() {
            let var = g_calloc(1, 4);
            wr::<u32>(var, sf + i as u32 * GUEST_FILE_SIZE);
            thunk_register_data(name, var);
        }
    }

    for n in ["fopen", "fopen64"] {
        r!(n, |p: Str, m: Str| -> u32 {
            let v = guest_proc_open(p);
            if v == -2 {
                return new_file(libc::fopen(p, m));
            }
            if v < 0 {
                return 0;
            }
            let f = libc::fdopen(v, c"r".as_ptr());
            if f.is_null() {
                libc::close(v);
            }
            new_file(f)
        });
    }
    r!("fdopen", |fd: i32, m: Str| -> u32 { new_file(libc::fdopen(fd, m)) });
    r!("tmpfile", | | -> u32 { new_file(libc::tmpfile()) });
    r!("freopen", |p: Str, m: Str, g: u32| -> u32 {
        let f = hf(g);
        if f.is_null() {
            return 0;
        }
        if libc::freopen(p, m, f).is_null() {
            drop_file(g);
            return 0;
        }
        g
    });
    r!("fclose", |g: u32| -> i32 {
        let f = drop_file(g);
        if f.is_null() {
            return if hf(g).is_null() { -1 } else { 0 };
        }
        g_free(g);
        libc::fclose(f)
    });
    r!("fread", |b: *mut c_void, s: usize, n: usize, g: u32| -> usize {
        let f = hf(g);
        if f.is_null() { 0 } else { libc::fread(b, s, n, f) }
    });
    r!("fwrite", |b: *const c_void, s: usize, n: usize, g: u32| -> usize {
        let f = hf(g);
        if f.is_null() {
            return 0;
        }
        #[cfg(target_os = "android")]
        if f == stdout || f == stderr {
            emit(f, std::slice::from_raw_parts(b as *const u8, s * n));
            return n;
        }
        libc::fwrite(b, s, n, f)
    });
    for n in ["fseek", "fseeko"] {
        r!(n, |g: u32, off: i32, wh: i32| -> i32 {
            let f = hf(g);
            if f.is_null() { -1 } else { libc::fseeko(f, off as libc::off_t, wh) }
        });
    }
    r!("fseeko64", |g: u32, off: i64, wh: i32| -> i32 {
        let f = hf(g);
        if f.is_null() { -1 } else { libc::fseeko(f, off as libc::off_t, wh) }
    });
    for n in ["ftell", "ftello"] {
        r!(n, |g: u32| -> i32 {
            let f = hf(g);
            if f.is_null() { -1 } else { libc::ftello(f) as i32 }
        });
    }
    r!("ftello64", |g: u32| -> i64 {
        let f = hf(g);
        if f.is_null() { -1 } else { libc::ftello(f) as i64 }
    });
    r!("rewind", |g: u32| {
        let f = hf(g);
        if !f.is_null() {
            libc::rewind(f);
        }
    });
    r!("fgetpos", |g: u32, pos: *mut i32| -> i32 {
        let f = hf(g);
        if f.is_null() {
            return -1;
        }
        *pos = libc::ftello(f) as i32;
        0
    });
    r!("fsetpos", |g: u32, pos: *const i32| -> i32 {
        let f = hf(g);
        if f.is_null() { -1 } else { libc::fseeko(f, *pos as libc::off_t, libc::SEEK_SET) }
    });
    r!("fflush", |g: u32| -> i32 {
        if g == 0 {
            return libc::fflush(std::ptr::null_mut());
        }
        let f = hf(g);
        if f.is_null() { -1 } else { libc::fflush(f) }
    });
    r!("feof", |g: u32| -> i32 {
        let f = hf(g);
        if f.is_null() { 1 } else { libc::feof(f) }
    });
    r!("ferror", |g: u32| -> i32 {
        let f = hf(g);
        if f.is_null() { 1 } else { libc::ferror(f) }
    });
    r!("clearerr", |g: u32| {
        let f = hf(g);
        if !f.is_null() {
            libc::clearerr(f);
        }
    });
    r!("fileno", |g: u32| -> i32 {
        let f = hf(g);
        if f.is_null() { -1 } else { libc::fileno(f) }
    });
    r!("fgets", |b: *mut c_char, n: i32, g: u32| -> *mut c_char {
        let f = hf(g);
        if f.is_null() { std::ptr::null_mut() } else { libc::fgets(b, n, f) }
    });
    // __srget: getc() macro refill
    for n in ["fgetc", "__srget", "getc"] {
        r!(n, |g: u32| -> i32 {
            let f = hf(g);
            if f.is_null() { EOF } else { libc::fgetc(f) }
        });
    }
    r!("__swbuf", |ch: i32, g: u32| -> i32 {
        let f = hf(g);
        if f.is_null() { EOF } else { libc::fputc(ch, f) }
    });
    r!("ungetc", |ch: i32, g: u32| -> i32 {
        let f = hf(g);
        if f.is_null() { EOF } else { libc::ungetc(ch, f) }
    });
    for n in ["fputc", "putc"] {
        r!(n, |ch: i32, g: u32| -> i32 {
            let f = hf(g);
            if f.is_null() {
                return EOF;
            }
            emit(f, &[ch as u8]);
            ch
        });
    }
    r!("fputs", |s: Str, g: u32| -> i32 {
        let f = hf(g);
        if f.is_null() {
            return EOF;
        }
        emit(f, cbytes(s));
        0
    });
    r!("puts", |s: Str| -> i32 {
        let mut v = cbytes(s).to_vec();
        v.push(b'\n');
        emit(stdout, &v);
        0
    });
    r!("putchar", |ch: i32| -> i32 {
        emit(stdout, &[ch as u8]);
        ch
    });
    reg("getchar", || -> i32 { EOF });
    r!("perror", |s: Str| {
        let e = crate::util::errno();
        let mut v = cbytes(s).to_vec();
        v.extend_from_slice(b": ");
        v.extend_from_slice(cbytes(libc::strerror(e)));
        v.push(b'\n');
        emit(stderr, &v);
    });
    reg("setvbuf", |_f: u32, _b: u32, _m: i32, _n: u32| -> i32 { 0 });
    reg("setbuf", |_f: u32, _b: u32| {});
    reg("setbuffer", |_f: u32, _b: u32, _n: u32| {});
    reg("flockfile", |_f: u32| {});
    reg("funlockfile", |_f: u32| {});

    // ---- printf family ----
    // Where the variadic arguments come from: the call itself or a va_list argument.
    fn va_of(a: &mut ArgReader, list: bool) -> VaSource {
        if list {
            VaSource::from_va_list(a.next32())
        } else {
            VaSource::from_args(a)
        }
    }
    // (name, takes a va_list, has the fortify "flag" argument)
    for (name, list, chk) in [("printf", false, false), ("vprintf", true, false)] {
        reg_raw(name, move |c| unsafe {
            let _ = chk;
            let mut a = ArgReader::new(c);
            let f: Str = a.ptr();
            let s = guest_format(f, &mut va_of(&mut a, list));
            emit(stdout, &s);
            c.set_ret32(s.len() as u32);
        });
    }
    for (name, list, chk) in [("fprintf", false, false), ("vfprintf", true, false), ("__fprintf_chk", false, true)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            let fp = hf(a.next32());
            if chk {
                a.next32();
            }
            let f: Str = a.ptr();
            let s = guest_format(f, &mut va_of(&mut a, list));
            emit(fp, &s);
            c.set_ret32(s.len() as u32);
        });
    }
    // fortified variants: (dst, [size,] flags, dst_size, fmt, ...)
    for (name, list, chk) in [("sprintf", false, false), ("vsprintf", true, false), ("__sprintf_chk", false, true), ("__vsprintf_chk", true, true)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            let d: *mut u8 = a.ptr();
            if chk {
                a.next32();
                a.next32();
            }
            let f: Str = a.ptr();
            let s = guest_format(f, &mut va_of(&mut a, list));
            c.set_ret32(store(d, 0, false, &s));
        });
    }
    for (name, list, chk) in [("snprintf", false, false), ("vsnprintf", true, false), ("__snprintf_chk", false, true), ("__vsnprintf_chk", true, true)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            let d: *mut u8 = a.ptr();
            let n = a.next32();
            if chk {
                a.next32();
                a.next32();
            }
            let f: Str = a.ptr();
            let s = guest_format(f, &mut va_of(&mut a, list));
            c.set_ret32(store(d, n as usize, true, &s));
        });
    }
    for (name, list) in [("asprintf", false), ("vasprintf", true)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            let out = a.next32();
            let f: Str = a.ptr();
            let s = guest_format(f, &mut va_of(&mut a, list));
            wr::<u32>(out, g_strdup_bytes(&s));
            c.set_ret32(s.len() as u32);
        });
    }

    // ---- scanf family ----
    for (name, list) in [("sscanf", false), ("vsscanf", true)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            let s: Str = a.ptr();
            let f: Str = a.ptr();
            let r = do_scan(ScanSrc::Str(s), f, &mut va_of(&mut a, list));
            c.set_ret32(r as u32);
        });
    }
    for (name, list) in [("fscanf", false), ("vfscanf", true)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            let fp = hf(a.next32());
            let f: Str = a.ptr();
            let r = if fp.is_null() { -1 } else { do_scan(ScanSrc::File(fp), f, &mut va_of(&mut a, list)) };
            c.set_ret32(r as u32);
        });
    }
}
