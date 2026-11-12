// SPDX-License-Identifier: GPL-2.0-or-later
//
// Functions and variables that Android's C library (bionic) exported before Android 5.0 and later
// removed or turned into inline code. Old libraries still import them.

#include <sys/file.h>
#include <unistd.h>

#include <cstring>

#include "guest_mem.h"
#include "thunk.h"

namespace a64b {

namespace {

// ------------------------------------------------------------------ SHA-1 (bionic's NetBSD sha1.c)
// typedef struct { uint32_t state[5]; uint32_t count[2]; uint8_t buffer[64]; } SHA1_CTX;
// count is the message length in bits: count[0] the low word, count[1] the high word.
struct GSha1 {
    u32 state[5];
    u32 count[2];
    u8 buffer[64];
};

u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1_block(u32 st[5], const u8* p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
}

void sha1_update(GSha1* ctx, const u8* data, u32 len) {
    u32 used = (ctx->count[0] >> 3) & 63;
    u64 bits = ((u64)ctx->count[1] << 32 | ctx->count[0]) + ((u64)len << 3);
    ctx->count[0] = (u32)bits;
    ctx->count[1] = (u32)(bits >> 32);
    while (len) {
        u32 n = 64 - used < len ? 64 - used : len;
        memcpy(ctx->buffer + used, data, n);
        used += n;
        data += n;
        len -= n;
        if (used == 64) {
            sha1_block(ctx->state, ctx->buffer);
            used = 0;
        }
    }
}

}  // namespace

void register_old_bionic_thunks() {
    // getpagesize() was inline code that read this variable.
    u32 page_size = g_calloc(1, 4), page_shift = g_calloc(1, 4);
    *g2h<u32>(page_size) = 4096;
    *g2h<u32>(page_shift) = 12;
    thunk_register_data("__page_size", page_size);
    thunk_register_data("__page_shift", page_shift);

    // Atomic counters of old bionic (sys/atomics.h). They return the previous value; cmpxchg returns
    // 0 on success.
    reg("__atomic_inc", +[](i32* p) -> i32 { return __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST); });
    reg("__atomic_dec", +[](i32* p) -> i32 { return __atomic_fetch_sub(p, 1, __ATOMIC_SEQ_CST); });
    reg("__atomic_swap", +[](i32 v, i32* p) -> i32 { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); });
    reg("__atomic_cmpxchg", +[](i32 old, i32 v, i32* p) -> int {
        return __atomic_compare_exchange_n(p, &old, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
    });

    reg("SHA1Init", +[](GSha1* ctx) {
        static const u32 init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        memcpy(ctx->state, init, sizeof(init));
        ctx->count[0] = ctx->count[1] = 0;
    });
    reg("SHA1Update", +[](GSha1* ctx, const u8* data, u32 len) { sha1_update(ctx, data, len); });
    reg("SHA1Final", +[](u8* digest, GSha1* ctx) {
        u8 length[8];
        for (int i = 0; i < 8; i++) length[i] = (u8)((i < 4 ? ctx->count[1] : ctx->count[0]) >> ((3 - (i & 3)) * 8));
        const u8 pad = 0x80, zero = 0;
        sha1_update(ctx, &pad, 1);
        while (((ctx->count[0] >> 3) & 63) != 56) sha1_update(ctx, &zero, 1);
        sha1_update(ctx, length, 8);
        for (int i = 0; i < 20; i++) digest[i] = (u8)(ctx->state[i / 4] >> ((3 - (i & 3)) * 8));
        memset(ctx, 0, sizeof(*ctx));
    });

    reg("flock", +[](int fd, int op) -> int { return flock(fd, op); });
    reg("getdtablesize", +[]() -> int {
        long n = sysconf(_SC_OPEN_MAX);
        return n > 0 && n < 65536 ? (int)n : 1024;
    });
}

}  // namespace a64b
