//! libz. z_stream contains pointers and "unsigned long" fields, so the 32-bit
//! guest layout is translated to a host z_stream around every call. The host
//! stream lives as long as the guest stream (keyed by the guest address).
// SPDX-License-Identifier: GPL-2.0-or-later
use std::collections::HashMap;
use std::os::raw::{c_char, c_int, c_ulong, c_void};
use std::sync::Mutex;

use crate::mem::*;
use crate::thunk::reg;
use crate::thunks::libc::intern_cstr;
use crate::util::lock;
use crate::zsys::*;

#[repr(C)]
struct GZStream {
    next_in: u32,
    avail_in: u32,
    total_in: u32,
    next_out: u32,
    avail_out: u32,
    total_out: u32,
    msg: u32,
    state: u32,
    zalloc: u32,
    zfree: u32,
    opaque: u32,
    data_type: i32,
    adler: u32,
    reserved: u32,
}
const _: () = assert!(std::mem::size_of::<GZStream>() == 56);

struct Tables {
    streams: HashMap<u32, usize>, // guest z_stream -> Box<z_stream>
    gz: HashMap<u32, usize>,      // guest handle -> gzFile
}
static T: Mutex<Option<Tables>> = Mutex::new(None);
fn tables<R>(f: impl FnOnce(&mut Tables) -> R) -> R {
    f(lock(&T).get_or_insert_with(|| Tables { streams: HashMap::new(), gz: HashMap::new() }))
}

fn find(gs: u32) -> *mut z_stream {
    tables(|t| t.streams.get(&gs).copied().unwrap_or(0)) as *mut z_stream
}

unsafe fn to_host(g: *const GZStream, z: *mut z_stream) {
    (*z).next_in = g2h::<u8>((*g).next_in);
    (*z).avail_in = (*g).avail_in;
    (*z).next_out = g2h::<u8>((*g).next_out);
    (*z).avail_out = (*g).avail_out;
    (*z).total_in = (*g).total_in as c_ulong;
    (*z).total_out = (*g).total_out as c_ulong;
}

unsafe fn to_guest(g: *mut GZStream, z: *const z_stream) {
    (*g).next_in = h2g((*z).next_in);
    (*g).avail_in = (*z).avail_in;
    (*g).next_out = h2g((*z).next_out);
    (*g).avail_out = (*z).avail_out;
    (*g).total_in = (*z).total_in as u32;
    (*g).total_out = (*z).total_out as u32;
    (*g).adler = (*z).adler as u32;
    (*g).data_type = (*z).data_type;
    (*g).msg = if (*z).msg.is_null() { 0 } else { intern_cstr((*z).msg) };
}

fn with_stream(gs: u32, f: impl FnOnce(*mut z_stream) -> c_int) -> c_int {
    let z = find(gs);
    if z.is_null() {
        return Z_STREAM_ERROR;
    }
    unsafe {
        let g = g2h::<GZStream>(gs);
        to_host(g, z);
        let r = f(z);
        to_guest(g, z);
        r
    }
}

fn init_stream(gs: u32, f: impl FnOnce(*mut z_stream) -> c_int) -> c_int {
    if gs == 0 {
        return Z_STREAM_ERROR;
    }
    unsafe {
        let z: *mut z_stream = Box::into_raw(Box::new(std::mem::zeroed()));
        let g = g2h::<GZStream>(gs);
        to_host(g, z);
        let r = f(z);
        to_guest(g, z);
        if r != Z_OK {
            drop(Box::from_raw(z));
            return r;
        }
        (*g).state = 0xA64B_0000; // non-NULL so guest-side "is initialised" checks pass
        tables(|t| {
            if let Some(old) = t.streams.insert(gs, z as usize) {
                drop(Box::from_raw(old as *mut z_stream)); // re-initialised without End
            }
        });
        r
    }
}

fn end_stream(gs: u32, f: impl FnOnce(*mut z_stream) -> c_int) -> c_int {
    let Some(z) = tables(|t| t.streams.remove(&gs)) else { return Z_STREAM_ERROR };
    let z = z as *mut z_stream;
    unsafe {
        let g = g2h::<GZStream>(gs);
        to_host(g, z);
        let r = f(z);
        to_guest(g, z);
        (*g).state = 0;
        drop(Box::from_raw(z));
        r
    }
}

fn gz(g: u32) -> gzFile {
    tables(|t| t.gz.get(&g).copied().unwrap_or(0)) as gzFile
}

fn gz_wrap(f: gzFile) -> u32 {
    if f.is_null() {
        return 0;
    }
    // zlib.h's gzgetc() macro reads the first fields of the handle: keep them zero.
    let g = g_calloc(1, 32);
    tables(|t| t.gz.insert(g, f as usize));
    g
}

pub fn register_zlib_thunks() {
    unsafe fn with_len(dlen: *mut u32, f: impl FnOnce(*mut c_ulong) -> c_int) -> c_int {
        let mut n = *dlen as c_ulong;
        let r = f(&mut n);
        *dlen = n as u32;
        r
    }
    reg("zlibVersion", || -> u32 { intern_cstr(unsafe { zlibVersion() }) });
    reg("zError", |e: i32| -> u32 { intern_cstr(unsafe { zError(e) }) });
    reg("crc32", |crc: u32, buf: *const u8, len: u32| -> u32 { unsafe { crc32(crc as c_ulong, buf, len) as u32 } });
    reg("adler32", |a: u32, buf: *const u8, len: u32| -> u32 { unsafe { adler32(a as c_ulong, buf, len) as u32 } });
    reg("crc32_combine", |a: u32, b: u32, len: i32| -> u32 { unsafe { crc32_combine(a as c_ulong, b as c_ulong, len as _) as u32 } });
    reg("adler32_combine", |a: u32, b: u32, len: i32| -> u32 { unsafe { adler32_combine(a as c_ulong, b as c_ulong, len as _) as u32 } });
    reg("compressBound", |n: u32| -> u32 { unsafe { compressBound(n as c_ulong) as u32 } });
    reg("compress", |d: *mut u8, dlen: *mut u32, s: *const u8, slen: u32| -> i32 { unsafe { with_len(dlen, |n| compress(d, n, s, slen as c_ulong)) } });
    reg("compress2", |d: *mut u8, dlen: *mut u32, s: *const u8, slen: u32, level: i32| -> i32 {
        unsafe { with_len(dlen, |n| compress2(d, n, s, slen as c_ulong, level)) }
    });
    reg("uncompress", |d: *mut u8, dlen: *mut u32, s: *const u8, slen: u32| -> i32 { unsafe { with_len(dlen, |n| uncompress(d, n, s, slen as c_ulong)) } });

    reg("inflateInit_", |s: u32, _v: u32, _sz: i32| -> i32 { init_stream(s, |z| unsafe { inflate_init2(z, MAX_WBITS) }) });
    reg("inflateInit2_", |s: u32, wbits: i32, _v: u32, _sz: i32| -> i32 { init_stream(s, |z| unsafe { inflate_init2(z, wbits) }) });
    reg("deflateInit_", |s: u32, level: i32, _v: u32, _sz: i32| -> i32 { init_stream(s, |z| unsafe { deflate_init2(z, level, 8, MAX_WBITS, 8, 0) }) });
    reg("deflateInit2_", |s: u32, level: i32, method: i32, wbits: i32, mem: i32, strat: i32, _v: u32, _sz: i32| -> i32 {
        init_stream(s, |z| unsafe { deflate_init2(z, level, method, wbits, mem, strat) })
    });
    reg("inflate", |s: u32, flush: i32| -> i32 { with_stream(s, |z| unsafe { inflate(z, flush) }) });
    reg("deflate", |s: u32, flush: i32| -> i32 { with_stream(s, |z| unsafe { deflate(z, flush) }) });
    reg("inflateEnd", |s: u32| -> i32 { end_stream(s, |z| unsafe { inflateEnd(z) }) });
    reg("deflateEnd", |s: u32| -> i32 { end_stream(s, |z| unsafe { deflateEnd(z) }) });
    reg("inflateReset", |s: u32| -> i32 { with_stream(s, |z| unsafe { inflateReset(z) }) });
    reg("inflateReset2", |s: u32, wbits: i32| -> i32 { with_stream(s, |z| unsafe { inflateReset2(z, wbits) }) });
    reg("deflateReset", |s: u32| -> i32 { with_stream(s, |z| unsafe { deflateReset(z) }) });
    reg("deflateParams", |s: u32, level: i32, strat: i32| -> i32 { with_stream(s, |z| unsafe { deflateParams(z, level, strat) }) });
    reg("inflateSetDictionary", |s: u32, d: *const u8, n: u32| -> i32 { with_stream(s, |z| unsafe { inflateSetDictionary(z, d, n) }) });
    reg("deflateSetDictionary", |s: u32, d: *const u8, n: u32| -> i32 { with_stream(s, |z| unsafe { deflateSetDictionary(z, d, n) }) });
    reg("inflateSync", |s: u32| -> i32 { with_stream(s, |z| unsafe { inflateSync(z) }) });
    reg("deflateBound", |s: u32, n: u32| -> u32 { unsafe { deflateBound(find(s), n as c_ulong) as u32 } });

    reg("gzopen", |path: *const c_char, mode: *const c_char| -> u32 { gz_wrap(unsafe { gzopen(path, mode) }) });
    reg("gzdopen", |fd: i32, mode: *const c_char| -> u32 { gz_wrap(unsafe { gzdopen(fd, mode) }) });
    reg("gzclose", |g: u32| -> i32 {
        let Some(f) = tables(|t| t.gz.remove(&g)) else { return Z_STREAM_ERROR };
        g_free(g);
        unsafe { gzclose(f as gzFile) }
    });
    reg("gzread", |g: u32, buf: *mut c_void, n: u32| -> i32 {
        let f = gz(g);
        if f.is_null() { -1 } else { unsafe { gzread(f, buf, n) } }
    });
    reg("gzwrite", |g: u32, buf: *const c_void, n: u32| -> i32 {
        let f = gz(g);
        if f.is_null() { 0 } else { unsafe { gzwrite(f, buf, n) } }
    });
    reg("gzputs", |g: u32, s: *const c_char| -> i32 {
        let f = gz(g);
        if f.is_null() { -1 } else { unsafe { gzputs(f, s) } }
    });
    reg("gzgets", |g: u32, buf: *mut c_char, n: i32| -> *mut c_char {
        let f = gz(g);
        if f.is_null() { std::ptr::null_mut() } else { unsafe { gzgets(f, buf, n) } }
    });
    for name in ["gzgetc", "gzgetc_"] {
        reg(name, |g: u32| -> i32 {
            let f = gz(g);
            if f.is_null() { -1 } else { unsafe { gzgetc(f) } }
        });
    }
    reg("gzputc", |g: u32, ch: i32| -> i32 {
        let f = gz(g);
        if f.is_null() { -1 } else { unsafe { gzputc(f, ch) } }
    });
    reg("gzeof", |g: u32| -> i32 {
        let f = gz(g);
        if f.is_null() { 1 } else { unsafe { gzeof(f) } }
    });
    reg("gzflush", |g: u32, fl: i32| -> i32 {
        let f = gz(g);
        if f.is_null() { Z_STREAM_ERROR } else { unsafe { gzflush(f, fl) } }
    });
    reg("gzrewind", |g: u32| -> i32 {
        let f = gz(g);
        if f.is_null() { -1 } else { unsafe { gzrewind(f) } }
    });
    reg("gzseek", |g: u32, off: i32, wh: i32| -> i32 {
        let f = gz(g);
        if f.is_null() { -1 } else { unsafe { gzseek(f, off as _, wh) as i32 } }
    });
    reg("gztell", |g: u32| -> i32 {
        let f = gz(g);
        if f.is_null() { -1 } else { unsafe { gztell(f) as i32 } }
    });
    reg("gzerror", |g: u32, err: *mut i32| -> u32 {
        let f = gz(g);
        unsafe {
            if f.is_null() {
                if !err.is_null() {
                    *err = Z_STREAM_ERROR;
                }
                return intern_cstr(c"invalid gzFile".as_ptr());
            }
            intern_cstr(gzerror(f, err))
        }
    });
}
