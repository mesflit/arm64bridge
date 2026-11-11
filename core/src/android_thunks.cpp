// liblog, libandroid (assets, native window), EGL.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <mutex>
#include <unordered_map>
#include <vector>

#include "jni_bridge.h"
#include "thunk.h"

#ifdef __ANDROID__
#include <EGL/egl.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#endif

namespace a64b {

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()

namespace {

void guest_log(int prio, const char* tag, const std::string& msg) {
#ifdef __ANDROID__
    __android_log_write(prio, tag ? tag : "guest", msg.c_str());
#else
    fprintf(stderr, "[guest %d/%s] %s\n", prio, tag ? tag : "", msg.c_str());
#endif
}

// 64-bit host pointers that the guest only stores and passes back (EGL handles,
// ANativeWindow*, AAsset*...) are replaced by small tokens.
std::mutex g_ptr_mu;
std::vector<void*> g_ptrs{nullptr};
std::unordered_map<void*, u32> g_ptr_lookup;

[[maybe_unused]] u32 ptr_token(void* p) {
    if (!p) return 0;
    std::lock_guard<std::mutex> lk(g_ptr_mu);
    auto it = g_ptr_lookup.find(p);
    if (it != g_ptr_lookup.end()) return it->second;
    u32 tok = 0x00A64000u + (u32)g_ptrs.size() * 4;
    g_ptrs.push_back(p);
    g_ptr_lookup[p] = tok;
    return tok;
}

[[maybe_unused]] void* ptr_host(u32 tok) {
    if (!tok) return nullptr;
    std::lock_guard<std::mutex> lk(g_ptr_mu);
    u32 idx = (tok - 0x00A64000u) / 4;
    if (tok < 0x00A64000u || idx >= g_ptrs.size()) return nullptr;
    return g_ptrs[idx];
}

[[maybe_unused]] void ptr_drop(u32 tok) {
    std::lock_guard<std::mutex> lk(g_ptr_mu);
    u32 idx = (tok - 0x00A64000u) / 4;
    if (tok < 0x00A64000u || idx >= g_ptrs.size()) return;
    g_ptr_lookup.erase(g_ptrs[idx]);
    g_ptrs[idx] = nullptr;
}

}  // namespace

void register_android_thunks() {
    // ---------------------------------------------------------------- liblog
    reg("__android_log_write", +[](int prio, const char* tag, const char* msg) -> int {
        guest_log(prio, tag, msg ? msg : "");
        return 1;
    });
    reg("__android_log_buf_write", +[](int, int prio, const char* tag, const char* msg) -> int {
        guest_log(prio, tag, msg ? msg : "");
        return 1;
    });
    RAW(__android_log_print) {
        ArgReader a(c); int prio = A32(); const char* tag = g2h<char>(A32()); const char* fmt = g2h<char>(A32());
        VaSource va(a);
        guest_log(prio, tag, guest_format(fmt, va));
        c.set_ret32(1);
    });
    RAW(__android_log_vprint) {
        ArgReader a(c); int prio = A32(); const char* tag = g2h<char>(A32()); const char* fmt = g2h<char>(A32());
        VaSource va(A32());
        guest_log(prio, tag, guest_format(fmt, va));
        c.set_ret32(1);
    });
    RAW(__android_log_buf_print) {
        ArgReader a(c); A32(); int prio = A32(); const char* tag = g2h<char>(A32()); const char* fmt = g2h<char>(A32());
        VaSource va(a);
        guest_log(prio, tag, guest_format(fmt, va));
        c.set_ret32(1);
    });
    RAW(__android_log_assert) {
        ArgReader a(c); u32 cond = A32(); const char* tag = g2h<char>(A32()); u32 fmt = A32();
        VaSource va(a);
        std::string msg = fmt ? guest_format(g2h<char>(fmt), va) : std::string(cond ? g2h<char>(cond) : "assertion failed");
        guest_log(7, tag, msg);
        fatal("guest __android_log_assert: %s", msg.c_str());
    });
    reg("__android_log_is_loggable", +[](int, const char*, int) -> int { return 1; });

    // Audio through OpenSL ES is not bridged yet: report "feature unsupported".
    reg("slCreateEngine", +[](u32*, u32, u32, u32, u32, u32) -> u32 {
        LOGW("OpenSL ES is not bridged yet; the app will run without native audio");
        return 12;  // SL_RESULT_FEATURE_UNSUPPORTED
    });

#ifdef __ANDROID__
    // ---------------------------------------------------------------- native window
    RAW(ANativeWindow_fromSurface) {
        ArgReader a(c); A32();
        jobject surface = jni_to_host(c, A32());
        c.set_ret32(ptr_token(ANativeWindow_fromSurface(jni_host_env(c), surface)));
    });
    reg("ANativeWindow_acquire", +[](u32 w) { if (auto* p = (ANativeWindow*)ptr_host(w)) ANativeWindow_acquire(p); });
    reg("ANativeWindow_release", +[](u32 w) { if (auto* p = (ANativeWindow*)ptr_host(w)) ANativeWindow_release(p); });
    reg("ANativeWindow_getWidth", +[](u32 w) -> int { auto* p = (ANativeWindow*)ptr_host(w); return p ? ANativeWindow_getWidth(p) : -1; });
    reg("ANativeWindow_getHeight", +[](u32 w) -> int { auto* p = (ANativeWindow*)ptr_host(w); return p ? ANativeWindow_getHeight(p) : -1; });
    reg("ANativeWindow_getFormat", +[](u32 w) -> int { auto* p = (ANativeWindow*)ptr_host(w); return p ? ANativeWindow_getFormat(p) : -1; });
    reg("ANativeWindow_setBuffersGeometry", +[](u32 w, int width, int height, int format) -> int {
        auto* p = (ANativeWindow*)ptr_host(w);
        return p ? ANativeWindow_setBuffersGeometry(p, width, height, format) : -1;
    });

    // ---------------------------------------------------------------- assets
    RAW(AAssetManager_fromJava) {
        ArgReader a(c); A32();
        jobject mgr = jni_to_host(c, A32());
        c.set_ret32(ptr_token(AAssetManager_fromJava(jni_host_env(c), mgr)));
    });
    reg("AAssetManager_open", +[](u32 mgr, const char* name, int mode) -> u32 {
        auto* m = (AAssetManager*)ptr_host(mgr);
        return m ? ptr_token(AAssetManager_open(m, name, mode)) : 0;
    });
    reg("AAsset_close", +[](u32 as) {
        if (auto* p = (AAsset*)ptr_host(as)) {
            AAsset_close(p);
            ptr_drop(as);
        }
    });
    reg("AAsset_read", +[](u32 as, void* buf, size_t n) -> int { auto* p = (AAsset*)ptr_host(as); return p ? AAsset_read(p, buf, n) : -1; });
    reg("AAsset_seek", +[](u32 as, i32 off, int wh) -> i32 { auto* p = (AAsset*)ptr_host(as); return p ? (i32)AAsset_seek(p, off, wh) : -1; });
    reg("AAsset_seek64", +[](u32 as, long long off, int wh) -> long long { auto* p = (AAsset*)ptr_host(as); return p ? (long long)AAsset_seek64(p, off, wh) : -1; });
    reg("AAsset_getLength", +[](u32 as) -> i32 { auto* p = (AAsset*)ptr_host(as); return p ? (i32)AAsset_getLength(p) : 0; });
    reg("AAsset_getLength64", +[](u32 as) -> long long { auto* p = (AAsset*)ptr_host(as); return p ? (long long)AAsset_getLength64(p) : 0; });
    reg("AAsset_getRemainingLength", +[](u32 as) -> i32 { auto* p = (AAsset*)ptr_host(as); return p ? (i32)AAsset_getRemainingLength(p) : 0; });
    reg("AAsset_getRemainingLength64", +[](u32 as) -> long long { auto* p = (AAsset*)ptr_host(as); return p ? (long long)AAsset_getRemainingLength64(p) : 0; });
    reg("AAsset_isAllocated", +[](u32) -> int { return 0; });
    reg("AAsset_getBuffer", +[](u32) -> u32 {
        LOGW("AAsset_getBuffer is not bridged (asset memory is outside the guest address space)");
        return 0;
    });
    reg("AAsset_openFileDescriptor", +[](u32 as, i32* start, i32* length) -> int {
        auto* p = (AAsset*)ptr_host(as);
        if (!p) return -1;
        off_t s = 0, l = 0;
        int fd = AAsset_openFileDescriptor(p, &s, &l);
        if (fd >= 0) {
            *start = (i32)s;
            *length = (i32)l;
        }
        return fd;
    });

    // ---------------------------------------------------------------- EGL
    reg("eglGetError", +[]() -> int { return eglGetError(); });
    reg("eglGetDisplay", +[](u32 native) -> u32 { return ptr_token(eglGetDisplay((EGLNativeDisplayType)ptr_host(native))); });
    reg("eglInitialize", +[](u32 d, EGLint* major, EGLint* minor) -> u32 { return eglInitialize(ptr_host(d), major, minor); });
    reg("eglTerminate", +[](u32 d) -> u32 { return eglTerminate(ptr_host(d)); });
    reg("eglQueryString", +[](u32 d, int name) -> u32 { return guest_intern_string(eglQueryString(ptr_host(d), name)); });
    reg("eglBindAPI", +[](u32 api) -> u32 { return eglBindAPI(api); });
    reg("eglSwapInterval", +[](u32 d, int n) -> u32 { return eglSwapInterval(ptr_host(d), n); });
    auto configs = +[](u32 d, const EGLint* attrs, u32* out, int size, EGLint* num, int choose) -> u32 {
        std::vector<EGLConfig> tmp((size_t)(size > 0 ? size : 0));
        EGLint n = 0;
        EGLBoolean ok = choose ? eglChooseConfig(ptr_host(d), attrs, out ? tmp.data() : nullptr, out ? size : 0, &n)
                               : eglGetConfigs(ptr_host(d), out ? tmp.data() : nullptr, out ? size : 0, &n);
        if (ok && out)
            for (int i = 0; i < n && i < size; i++) out[i] = ptr_token(tmp[i]);
        if (num) *num = n;
        return ok;
    };
    static auto s_configs = configs;
    reg("eglChooseConfig", +[](u32 d, const EGLint* attrs, u32* out, int size, EGLint* num) -> u32 { return s_configs(d, attrs, out, size, num, 1); });
    reg("eglGetConfigs", +[](u32 d, u32* out, int size, EGLint* num) -> u32 { return s_configs(d, nullptr, out, size, num, 0); });
    reg("eglGetConfigAttrib", +[](u32 d, u32 cfg, int attr, EGLint* value) -> u32 { return eglGetConfigAttrib(ptr_host(d), ptr_host(cfg), attr, value); });
    reg("eglCreateWindowSurface", +[](u32 d, u32 cfg, u32 win, const EGLint* attrs) -> u32 {
        return ptr_token(eglCreateWindowSurface(ptr_host(d), ptr_host(cfg), (EGLNativeWindowType)ptr_host(win), attrs));
    });
    reg("eglCreatePbufferSurface", +[](u32 d, u32 cfg, const EGLint* attrs) -> u32 {
        return ptr_token(eglCreatePbufferSurface(ptr_host(d), ptr_host(cfg), attrs));
    });
    reg("eglDestroySurface", +[](u32 d, u32 s) -> u32 { return eglDestroySurface(ptr_host(d), ptr_host(s)); });
    reg("eglQuerySurface", +[](u32 d, u32 s, int attr, EGLint* value) -> u32 { return eglQuerySurface(ptr_host(d), ptr_host(s), attr, value); });
    reg("eglSurfaceAttrib", +[](u32 d, u32 s, int attr, int value) -> u32 { return eglSurfaceAttrib(ptr_host(d), ptr_host(s), attr, value); });
    reg("eglCreateContext", +[](u32 d, u32 cfg, u32 share, const EGLint* attrs) -> u32 {
        return ptr_token(eglCreateContext(ptr_host(d), ptr_host(cfg), ptr_host(share), attrs));
    });
    reg("eglDestroyContext", +[](u32 d, u32 ctx) -> u32 { return eglDestroyContext(ptr_host(d), ptr_host(ctx)); });
    reg("eglMakeCurrent", +[](u32 d, u32 draw, u32 read, u32 ctx) -> u32 {
        return eglMakeCurrent(ptr_host(d), ptr_host(draw), ptr_host(read), ptr_host(ctx));
    });
    reg("eglSwapBuffers", +[](u32 d, u32 s) -> u32 { return eglSwapBuffers(ptr_host(d), ptr_host(s)); });
    reg("eglGetCurrentContext", +[]() -> u32 { return ptr_token(eglGetCurrentContext()); });
    reg("eglGetCurrentDisplay", +[]() -> u32 { return ptr_token(eglGetCurrentDisplay()); });
    reg("eglGetCurrentSurface", +[](int which) -> u32 { return ptr_token(eglGetCurrentSurface(which)); });
    reg("eglQueryContext", +[](u32 d, u32 ctx, int attr, EGLint* value) -> u32 { return eglQueryContext(ptr_host(d), ptr_host(ctx), attr, value); });
    reg("eglWaitGL", +[]() -> u32 { return eglWaitGL(); });
    reg("eglWaitNative", +[](int engine) -> u32 { return eglWaitNative(engine); });
    reg("eglReleaseThread", +[]() -> u32 { return eglReleaseThread(); });
    reg("eglGetProcAddress", +[](const char* name) -> u32 { return thunk_resolve(name); });
#endif
}

}  // namespace a64b
