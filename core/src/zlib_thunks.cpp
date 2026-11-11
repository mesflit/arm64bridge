// libz. z_stream contains pointers and "unsigned long" fields, so the 32-bit
// guest layout is translated to a host z_stream around every call. The host
// stream lives as long as the guest stream (keyed by the guest address).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <zlib.h>

#include <mutex>
#include <unordered_map>

#include "thunk.h"

namespace a64b {

namespace {

struct GZStream {
    u32 next_in, avail_in, total_in;
    u32 next_out, avail_out, total_out;
    u32 msg, state, zalloc, zfree, opaque;
    i32 data_type;
    u32 adler, reserved;
};
static_assert(sizeof(GZStream) == 56, "guest z_stream layout");

std::mutex g_mu;
std::unordered_map<u32, z_stream*> g_streams;
std::unordered_map<u32, gzFile> g_gz;

z_stream* find(u32 gs) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_streams.find(gs);
    return it == g_streams.end() ? nullptr : it->second;
}

void to_host(const GZStream* g, z_stream* z) {
    z->next_in = g2h<Bytef>(g->next_in);
    z->avail_in = g->avail_in;
    z->next_out = g2h<Bytef>(g->next_out);
    z->avail_out = g->avail_out;
    z->total_in = g->total_in;
    z->total_out = g->total_out;
}

void to_guest(GZStream* g, const z_stream* z) {
    g->next_in = h2g(z->next_in);
    g->avail_in = z->avail_in;
    g->next_out = h2g(z->next_out);
    g->avail_out = z->avail_out;
    g->total_in = (u32)z->total_in;
    g->total_out = (u32)z->total_out;
    g->adler = (u32)z->adler;
    g->data_type = z->data_type;
    g->msg = z->msg ? guest_intern_string(z->msg) : 0;
}

template <class F>
int with_stream(u32 gs, F f) {
    z_stream* z = find(gs);
    if (!z) return Z_STREAM_ERROR;
    GZStream* g = g2h<GZStream>(gs);
    to_host(g, z);
    int r = f(z);
    to_guest(g, z);
    return r;
}

template <class F>
int init_stream(u32 gs, F f) {
    if (!gs) return Z_STREAM_ERROR;
    auto* z = new z_stream();
    GZStream* g = g2h<GZStream>(gs);
    to_host(g, z);
    z->zalloc = Z_NULL;
    z->zfree = Z_NULL;
    z->opaque = Z_NULL;
    int r = f(z);
    to_guest(g, z);
    if (r != Z_OK) {
        delete z;
        return r;
    }
    g->state = 0xA64B0000;  // non-NULL so guest-side "is initialised" checks pass
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_streams.find(gs);
    if (it != g_streams.end()) delete it->second;  // re-initialised without End
    g_streams[gs] = z;
    return r;
}

template <class F>
int end_stream(u32 gs, F f) {
    z_stream* z = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_streams.find(gs);
        if (it == g_streams.end()) return Z_STREAM_ERROR;
        z = it->second;
        g_streams.erase(it);
    }
    GZStream* g = g2h<GZStream>(gs);
    to_host(g, z);
    int r = f(z);
    to_guest(g, z);
    g->state = 0;
    delete z;
    return r;
}

gzFile gz(u32 g) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_gz.find(g);
    return it == g_gz.end() ? nullptr : it->second;
}

u32 gz_wrap(gzFile f) {
    if (!f) return 0;
    // zlib.h's gzgetc() macro reads the first fields of the handle: keep them zero.
    u32 g = g_calloc(1, 32);
    std::lock_guard<std::mutex> lk(g_mu);
    g_gz[g] = f;
    return g;
}

}  // namespace

void register_zlib_thunks() {
    reg("zlibVersion", +[]() -> u32 { return guest_intern_string(zlibVersion()); });
    reg("zError", +[](int e) -> u32 { return guest_intern_string(zError(e)); });
    reg("crc32", +[](u32 crc, const Bytef* buf, u32 len) -> u32 { return (u32)crc32(crc, buf, len); });
    reg("adler32", +[](u32 a, const Bytef* buf, u32 len) -> u32 { return (u32)adler32(a, buf, len); });
    reg("crc32_combine", +[](u32 a, u32 b, i32 len) -> u32 { return (u32)crc32_combine(a, b, len); });
    reg("adler32_combine", +[](u32 a, u32 b, i32 len) -> u32 { return (u32)adler32_combine(a, b, len); });
    reg("compressBound", +[](u32 n) -> u32 { return (u32)compressBound(n); });
    reg("compress", +[](Bytef* d, u32* dlen, const Bytef* s, u32 slen) -> int {
        uLongf n = *dlen;
        int r = compress(d, &n, s, slen);
        *dlen = (u32)n;
        return r;
    });
    reg("compress2", +[](Bytef* d, u32* dlen, const Bytef* s, u32 slen, int level) -> int {
        uLongf n = *dlen;
        int r = compress2(d, &n, s, slen, level);
        *dlen = (u32)n;
        return r;
    });
    reg("uncompress", +[](Bytef* d, u32* dlen, const Bytef* s, u32 slen) -> int {
        uLongf n = *dlen;
        int r = uncompress(d, &n, s, slen);
        *dlen = (u32)n;
        return r;
    });

    reg("inflateInit_", +[](u32 s, const char*, int) -> int { return init_stream(s, [](z_stream* z) { return inflateInit(z); }); });
    reg("inflateInit2_", +[](u32 s, int wbits, const char*, int) -> int {
        return init_stream(s, [wbits](z_stream* z) { return inflateInit2(z, wbits); });
    });
    reg("deflateInit_", +[](u32 s, int level, const char*, int) -> int {
        return init_stream(s, [level](z_stream* z) { return deflateInit(z, level); });
    });
    reg("deflateInit2_", +[](u32 s, int level, int method, int wbits, int mem, int strat, const char*, int) -> int {
        return init_stream(s, [=](z_stream* z) { return deflateInit2(z, level, method, wbits, mem, strat); });
    });
    reg("inflate", +[](u32 s, int flush) -> int { return with_stream(s, [flush](z_stream* z) { return inflate(z, flush); }); });
    reg("deflate", +[](u32 s, int flush) -> int { return with_stream(s, [flush](z_stream* z) { return deflate(z, flush); }); });
    reg("inflateEnd", +[](u32 s) -> int { return end_stream(s, [](z_stream* z) { return inflateEnd(z); }); });
    reg("deflateEnd", +[](u32 s) -> int { return end_stream(s, [](z_stream* z) { return deflateEnd(z); }); });
    reg("inflateReset", +[](u32 s) -> int { return with_stream(s, [](z_stream* z) { return inflateReset(z); }); });
    reg("inflateReset2", +[](u32 s, int wbits) -> int { return with_stream(s, [wbits](z_stream* z) { return inflateReset2(z, wbits); }); });
    reg("deflateReset", +[](u32 s) -> int { return with_stream(s, [](z_stream* z) { return deflateReset(z); }); });
    reg("deflateParams", +[](u32 s, int level, int strat) -> int {
        return with_stream(s, [=](z_stream* z) { return deflateParams(z, level, strat); });
    });
    reg("inflateSetDictionary", +[](u32 s, const Bytef* d, u32 n) -> int {
        return with_stream(s, [=](z_stream* z) { return inflateSetDictionary(z, d, n); });
    });
    reg("deflateSetDictionary", +[](u32 s, const Bytef* d, u32 n) -> int {
        return with_stream(s, [=](z_stream* z) { return deflateSetDictionary(z, d, n); });
    });
    reg("inflateSync", +[](u32 s) -> int { return with_stream(s, [](z_stream* z) { return inflateSync(z); }); });
    reg("deflateBound", +[](u32 s, u32 n) -> u32 {
        z_stream* z = find(s);
        return (u32)deflateBound(z, n);
    });

    reg("gzopen", +[](const char* path, const char* mode) -> u32 { return gz_wrap(gzopen(path, mode)); });
    reg("gzdopen", +[](int fd, const char* mode) -> u32 { return gz_wrap(gzdopen(fd, mode)); });
    reg("gzclose", +[](u32 g) -> int {
        gzFile f;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_gz.find(g);
            if (it == g_gz.end()) return Z_STREAM_ERROR;
            f = it->second;
            g_gz.erase(it);
        }
        g_free(g);
        return gzclose(f);
    });
    reg("gzread", +[](u32 g, void* buf, u32 n) -> int { gzFile f = gz(g); return f ? gzread(f, buf, n) : -1; });
    reg("gzwrite", +[](u32 g, const void* buf, u32 n) -> int { gzFile f = gz(g); return f ? gzwrite(f, buf, n) : 0; });
    reg("gzputs", +[](u32 g, const char* s) -> int { gzFile f = gz(g); return f ? gzputs(f, s) : -1; });
    reg("gzgets", +[](u32 g, char* buf, int n) -> char* { gzFile f = gz(g); return f ? gzgets(f, buf, n) : nullptr; });
    reg("gzgetc", +[](u32 g) -> int { gzFile f = gz(g); return f ? gzgetc(f) : -1; });
    reg("gzgetc_", +[](u32 g) -> int { gzFile f = gz(g); return f ? gzgetc(f) : -1; });
    reg("gzputc", +[](u32 g, int ch) -> int { gzFile f = gz(g); return f ? gzputc(f, ch) : -1; });
    reg("gzeof", +[](u32 g) -> int { gzFile f = gz(g); return f ? gzeof(f) : 1; });
    reg("gzflush", +[](u32 g, int fl) -> int { gzFile f = gz(g); return f ? gzflush(f, fl) : Z_STREAM_ERROR; });
    reg("gzrewind", +[](u32 g) -> int { gzFile f = gz(g); return f ? gzrewind(f) : -1; });
    reg("gzseek", +[](u32 g, i32 off, int wh) -> i32 { gzFile f = gz(g); return f ? (i32)gzseek(f, off, wh) : -1; });
    reg("gztell", +[](u32 g) -> i32 { gzFile f = gz(g); return f ? (i32)gztell(f) : -1; });
    reg("gzerror", +[](u32 g, int* err) -> u32 {
        gzFile f = gz(g);
        if (!f) {
            if (err) *err = Z_STREAM_ERROR;
            return guest_intern_string("invalid gzFile");
        }
        return guest_intern_string(gzerror(f, err));
    });
}

}  // namespace a64b
