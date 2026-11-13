//! Functions and variables that Android's C library (bionic) exported before Android 5.0 and later
//! removed or turned into inline code. Old libraries still import them.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::sync::atomic::{AtomicI32, Ordering};

use crate::mem::*;
use crate::thunk::{reg, thunk_register_data};

// ------------------------------------------------------------------ SHA-1 (bionic's NetBSD sha1.c)
// typedef struct { uint32_t state[5]; uint32_t count[2]; uint8_t buffer[64]; } SHA1_CTX;
// count is the message length in bits: count[0] the low word, count[1] the high word.
#[repr(C)]
struct GSha1 {
    state: [u32; 5],
    count: [u32; 2],
    buffer: [u8; 64],
}

fn sha1_block(st: &mut [u32; 5], p: &[u8; 64]) {
    let mut w = [0u32; 80];
    for i in 0..16 {
        w[i] = u32::from_be_bytes([p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]]);
    }
    for i in 16..80 {
        w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
    }
    let (mut a, mut b, mut c, mut d, mut e) = (st[0], st[1], st[2], st[3], st[4]);
    for (i, &wi) in w.iter().enumerate() {
        let (f, k) = match i {
            0..=19 => ((b & c) | (!b & d), 0x5A82_7999),
            20..=39 => (b ^ c ^ d, 0x6ED9_EBA1),
            40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1B_BCDC),
            _ => (b ^ c ^ d, 0xCA62_C1D6u32),
        };
        let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(wi);
        e = d;
        d = c;
        c = b.rotate_left(30);
        b = a;
        a = t;
    }
    for (s, v) in st.iter_mut().zip([a, b, c, d, e]) {
        *s = s.wrapping_add(v);
    }
}

fn sha1_update(ctx: &mut GSha1, mut data: &[u8]) {
    let mut used = ((ctx.count[0] >> 3) & 63) as usize;
    let bits = (((ctx.count[1] as u64) << 32) | ctx.count[0] as u64).wrapping_add((data.len() as u64) << 3);
    ctx.count[0] = bits as u32;
    ctx.count[1] = (bits >> 32) as u32;
    while !data.is_empty() {
        let n = (64 - used).min(data.len());
        ctx.buffer[used..used + n].copy_from_slice(&data[..n]);
        used += n;
        data = &data[n..];
        if used == 64 {
            let block = ctx.buffer;
            sha1_block(&mut ctx.state, &block);
            used = 0;
        }
    }
}

pub fn register_old_bionic_thunks() {
    // getpagesize() was inline code that read this variable.
    unsafe {
        let (page_size, page_shift) = (g_calloc(1, 4), g_calloc(1, 4));
        wr::<u32>(page_size, 4096);
        wr::<u32>(page_shift, 12);
        thunk_register_data("__page_size", page_size);
        thunk_register_data("__page_shift", page_shift);
    }

    // Atomic counters of old bionic (sys/atomics.h). They return the previous value; cmpxchg returns
    // 0 on success.
    let o = Ordering::SeqCst;
    reg("__atomic_inc", move |p: *mut AtomicI32| -> i32 { unsafe { (*p).fetch_add(1, o) } });
    reg("__atomic_dec", move |p: *mut AtomicI32| -> i32 { unsafe { (*p).fetch_sub(1, o) } });
    reg("__atomic_swap", move |v: i32, p: *mut AtomicI32| -> i32 { unsafe { (*p).swap(v, o) } });
    reg("__atomic_cmpxchg", move |old: i32, v: i32, p: *mut AtomicI32| -> i32 { unsafe { (*p).compare_exchange(old, v, o, o).is_err() as i32 } });

    reg("SHA1Init", |ctx: *mut GSha1| unsafe {
        (*ctx).state = [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476, 0xC3D2_E1F0];
        (*ctx).count = [0, 0];
    });
    reg("SHA1Update", |ctx: *mut GSha1, data: *const u8, len: u32| unsafe { sha1_update(&mut *ctx, std::slice::from_raw_parts(data, len as usize)) });
    reg("SHA1Final", |digest: *mut u8, ctx: *mut GSha1| unsafe {
        let ctx = &mut *ctx;
        let mut length = [0u8; 8];
        length[..4].copy_from_slice(&ctx.count[1].to_be_bytes());
        length[4..].copy_from_slice(&ctx.count[0].to_be_bytes());
        sha1_update(ctx, &[0x80]);
        while ((ctx.count[0] >> 3) & 63) != 56 {
            sha1_update(ctx, &[0]);
        }
        sha1_update(ctx, &length);
        for i in 0..20 {
            *digest.add(i) = (ctx.state[i / 4] >> ((3 - (i & 3)) * 8)) as u8;
        }
        std::ptr::write_bytes(ctx as *mut GSha1, 0, 1);
    });

    reg("flock", |fd: i32, op: i32| -> i32 { unsafe { libc::flock(fd, op) } });
    reg("getdtablesize", || -> i32 {
        let n = unsafe { libc::sysconf(libc::_SC_OPEN_MAX) };
        if n > 0 && n < 65536 { n as i32 } else { 1024 }
    });
}
