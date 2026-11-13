//! The host's zlib (libz), declared by hand: only what the bridge uses.
// SPDX-License-Identifier: GPL-2.0-or-later
#![allow(non_camel_case_types)]
use std::os::raw::{c_char, c_int, c_long, c_uint, c_ulong, c_void};

#[repr(C)]
pub struct z_stream {
    pub next_in: *const u8,
    pub avail_in: c_uint,
    pub total_in: c_ulong,
    pub next_out: *mut u8,
    pub avail_out: c_uint,
    pub total_out: c_ulong,
    pub msg: *const c_char,
    pub state: *mut c_void,
    pub zalloc: *mut c_void,
    pub zfree: *mut c_void,
    pub opaque: *mut c_void,
    pub data_type: c_int,
    pub adler: c_ulong,
    pub reserved: c_ulong,
}

pub const Z_OK: c_int = 0;
pub const Z_STREAM_END: c_int = 1;
pub const Z_FINISH: c_int = 4;
pub const Z_STREAM_ERROR: c_int = -2;
pub const Z_MEM_ERROR: c_int = -4;
pub const MAX_WBITS: c_int = 15;

pub type gzFile = *mut c_void;

#[link(name = "z")]
extern "C" {
    pub fn zlibVersion() -> *const c_char;
    pub fn inflateInit2_(strm: *mut z_stream, window_bits: c_int, version: *const c_char, stream_size: c_int) -> c_int;
    pub fn inflate(strm: *mut z_stream, flush: c_int) -> c_int;
    pub fn inflateEnd(strm: *mut z_stream) -> c_int;
    pub fn inflateReset(strm: *mut z_stream) -> c_int;
    pub fn inflateReset2(strm: *mut z_stream, window_bits: c_int) -> c_int;
    pub fn inflateSetDictionary(strm: *mut z_stream, dict: *const u8, len: c_uint) -> c_int;
    pub fn inflateSync(strm: *mut z_stream) -> c_int;
    pub fn deflateInit2_(strm: *mut z_stream, level: c_int, method: c_int, window_bits: c_int, mem_level: c_int, strategy: c_int, version: *const c_char, stream_size: c_int) -> c_int;
    pub fn deflate(strm: *mut z_stream, flush: c_int) -> c_int;
    pub fn deflateEnd(strm: *mut z_stream) -> c_int;
    pub fn deflateReset(strm: *mut z_stream) -> c_int;
    pub fn deflateBound(strm: *mut z_stream, source_len: c_ulong) -> c_ulong;
    pub fn deflateParams(strm: *mut z_stream, level: c_int, strategy: c_int) -> c_int;
    pub fn deflateSetDictionary(strm: *mut z_stream, dict: *const u8, len: c_uint) -> c_int;
    pub fn compress(dest: *mut u8, dest_len: *mut c_ulong, source: *const u8, source_len: c_ulong) -> c_int;
    pub fn compress2(dest: *mut u8, dest_len: *mut c_ulong, source: *const u8, source_len: c_ulong, level: c_int) -> c_int;
    pub fn compressBound(source_len: c_ulong) -> c_ulong;
    pub fn uncompress(dest: *mut u8, dest_len: *mut c_ulong, source: *const u8, source_len: c_ulong) -> c_int;
    pub fn crc32(crc: c_ulong, buf: *const u8, len: c_uint) -> c_ulong;
    pub fn adler32(adler: c_ulong, buf: *const u8, len: c_uint) -> c_ulong;
    pub fn crc32_combine(a: c_ulong, b: c_ulong, len: c_long) -> c_ulong;
    pub fn adler32_combine(a: c_ulong, b: c_ulong, len: c_long) -> c_ulong;
    pub fn get_crc_table() -> *const u32;
    pub fn zError(err: c_int) -> *const c_char;
    pub fn gzopen(path: *const c_char, mode: *const c_char) -> gzFile;
    pub fn gzdopen(fd: c_int, mode: *const c_char) -> gzFile;
    pub fn gzread(f: gzFile, buf: *mut c_void, len: c_uint) -> c_int;
    pub fn gzwrite(f: gzFile, buf: *const c_void, len: c_uint) -> c_int;
    pub fn gzclose(f: gzFile) -> c_int;
    pub fn gzeof(f: gzFile) -> c_int;
    pub fn gzgets(f: gzFile, buf: *mut c_char, len: c_int) -> *mut c_char;
    pub fn gzputs(f: gzFile, s: *const c_char) -> c_int;
    pub fn gzgetc(f: gzFile) -> c_int;
    pub fn gzputc(f: gzFile, c: c_int) -> c_int;
    pub fn gzflush(f: gzFile, flush: c_int) -> c_int;
    pub fn gzseek(f: gzFile, offset: c_long, whence: c_int) -> c_long;
    pub fn gztell(f: gzFile) -> c_long;
    pub fn gzrewind(f: gzFile) -> c_int;
    pub fn gzerror(f: gzFile, errnum: *mut c_int) -> *const c_char;
    pub fn gzclearerr(f: gzFile);
    pub fn gzsetparams(f: gzFile, level: c_int, strategy: c_int) -> c_int;
}

pub unsafe fn inflate_init2(strm: *mut z_stream, window_bits: c_int) -> c_int {
    inflateInit2_(strm, window_bits, zlibVersion(), std::mem::size_of::<z_stream>() as c_int)
}
pub unsafe fn deflate_init2(strm: *mut z_stream, level: c_int, method: c_int, window_bits: c_int, mem_level: c_int, strategy: c_int) -> c_int {
    deflateInit2_(strm, level, method, window_bits, mem_level, strategy, zlibVersion(), std::mem::size_of::<z_stream>() as c_int)
}

/// Inflates a raw deflate stream whose uncompressed size is known.
pub fn inflate_raw(input: &[u8], out_size: usize) -> Option<Vec<u8>> {
    unsafe {
        let mut out = vec![0u8; out_size];
        let mut zs: z_stream = std::mem::zeroed();
        if inflate_init2(&mut zs, -MAX_WBITS) != Z_OK {
            return None;
        }
        zs.next_in = input.as_ptr();
        zs.avail_in = input.len() as c_uint;
        zs.next_out = out.as_mut_ptr();
        zs.avail_out = out_size as c_uint;
        let r = inflate(&mut zs, Z_FINISH);
        let total = zs.total_out as usize;
        inflateEnd(&mut zs);
        (r == Z_STREAM_END && total == out_size).then_some(out)
    }
}
