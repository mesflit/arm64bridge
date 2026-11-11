// liblog, libandroid (assets, native window), EGL.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <sys/resource.h>
#include <unistd.h>

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
#include <android/configuration.h>
#include <android/looper.h>
#include <android/sensor.h>
#endif

namespace a64b {

#define RAW(name) reg_raw(#name, [](Cpu& c)
#define A32() a.next32()

namespace {

void guest_log(int prio, const char* tag, const std::string& msg) {
    log_tagged(prio, tag && *tag ? tag : "guest", msg.c_str());
}

// 64-bit host pointers that the guest only stores and passes back (EGL handles,
// ANativeWindow*, AAsset*...) are replaced by small tokens.
std::mutex g_ptr_mu;
std::vector<void*> g_ptrs{nullptr};
std::unordered_map<void*, u32> g_ptr_lookup;
[[maybe_unused]] std::unordered_map<u32, u32> g_asset_buffers;  // AAsset token -> guest copy

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

    register_opensl_thunks();

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
            u32 copy = 0;
            {
                std::lock_guard<std::mutex> lk(g_ptr_mu);
                auto it = g_asset_buffers.find(as);
                if (it != g_asset_buffers.end()) {
                    copy = it->second;
                    g_asset_buffers.erase(it);
                }
            }
            if (copy) g_free(copy);
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

    // ---------------------------------------------------------------- asset directories
    struct GAssetDir {
        AAssetDir* dir;
        u32 name;  // guest copy of the last returned file name
    };
    reg("AAssetManager_openDir", +[](u32 mgr, const char* path) -> u32 {
        auto* m = (AAssetManager*)ptr_host(mgr);
        AAssetDir* d = m ? AAssetManager_openDir(m, path ? path : "") : nullptr;
        if (!d) return 0;
        return ptr_token(new GAssetDir{d, g_malloc(1024)});
    });
    reg("AAssetDir_getNextFileName", +[](u32 tok) -> u32 {
        auto* g = (GAssetDir*)ptr_host(tok);
        const char* n = g ? AAssetDir_getNextFileName(g->dir) : nullptr;
        if (!n) return 0;
        snprintf(g2h<char>(g->name), 1024, "%s", n);
        return g->name;
    });
    reg("AAssetDir_rewind", +[](u32 tok) { if (auto* g = (GAssetDir*)ptr_host(tok)) AAssetDir_rewind(g->dir); });
    reg("AAssetDir_close", +[](u32 tok) {
        auto* g = (GAssetDir*)ptr_host(tok);
        if (!g) return;
        AAssetDir_close(g->dir);
        g_free(g->name);
        ptr_drop(tok);
        delete g;
    });
    reg("AAsset_getBuffer", +[](u32 as) -> u32 {
        // The asset's own buffer is outside guest memory: hand out a guest copy, freed by AAsset_close.
        auto* p = (AAsset*)ptr_host(as);
        if (!p) return 0;
        std::lock_guard<std::mutex> lk(g_ptr_mu);
        auto it = g_asset_buffers.find(as);
        if (it != g_asset_buffers.end()) return it->second;
        const void* buf = AAsset_getBuffer(p);
        off64_t len = AAsset_getLength64(p);
        if (!buf || len < 0 || len > 0x7fffffff) return 0;
        u32 g = g_malloc((u32)len + 1);
        if (!g) return 0;
        memcpy(g2h(g), buf, (size_t)len);
        g_asset_buffers[as] = g;
        return g;
    });

    // ---------------------------------------------------------------- configuration
    reg("AConfiguration_new", +[]() -> u32 { return ptr_token(AConfiguration_new()); });
    reg("AConfiguration_delete", +[](u32 c) {
        if (auto* p = (AConfiguration*)ptr_host(c)) {
            AConfiguration_delete(p);
            ptr_drop(c);
        }
    });
    reg("AConfiguration_fromAssetManager", +[](u32 c, u32 mgr) {
        auto* p = (AConfiguration*)ptr_host(c);
        auto* m = (AAssetManager*)ptr_host(mgr);
        if (p && m) AConfiguration_fromAssetManager(p, m);
    });
    reg("AConfiguration_getLanguage", +[](u32 c, char* out) { if (auto* p = (AConfiguration*)ptr_host(c)) AConfiguration_getLanguage(p, out); });
    reg("AConfiguration_getCountry", +[](u32 c, char* out) { if (auto* p = (AConfiguration*)ptr_host(c)) AConfiguration_getCountry(p, out); });
#define ACFG(name) reg("AConfiguration_" #name, +[](u32 c) -> int { auto* p = (AConfiguration*)ptr_host(c); return p ? (int)AConfiguration_##name(p) : 0; });
    ACFG(getMcc) ACFG(getMnc) ACFG(getOrientation) ACFG(getTouchscreen) ACFG(getDensity) ACFG(getKeyboard)
    ACFG(getNavigation) ACFG(getKeysHidden) ACFG(getNavHidden) ACFG(getSdkVersion) ACFG(getScreenSize)
    ACFG(getScreenLong) ACFG(getUiModeType) ACFG(getUiModeNight) ACFG(getScreenWidthDp) ACFG(getScreenHeightDp)
    ACFG(getSmallestScreenWidthDp) ACFG(getLayoutDirection)
#undef ACFG

    // ---------------------------------------------------------------- loopers and sensors
    reg("ALooper_prepare", +[](int opts) -> u32 { return ptr_token(ALooper_prepare(opts)); });
    reg("ALooper_forThread", +[]() -> u32 { return ptr_token(ALooper_forThread()); });
    reg("ALooper_acquire", +[](u32 l) { if (auto* p = (ALooper*)ptr_host(l)) ALooper_acquire(p); });
    reg("ALooper_release", +[](u32 l) { if (auto* p = (ALooper*)ptr_host(l)) ALooper_release(p); });
    reg("ALooper_wake", +[](u32 l) { if (auto* p = (ALooper*)ptr_host(l)) ALooper_wake(p); });
    reg("ALooper_pollOnce", +[](int timeout, i32* fd, i32* events, u32* data) -> int {
        int f = 0, e = 0;
        void* d = nullptr;
        int r = ALooper_pollOnce(timeout, &f, &e, &d);
        if (fd) *fd = f;
        if (events) *events = e;
        if (data) *data = (u32)(uintptr_t)d;
        return r;
    });
    reg("ALooper_pollAll", +[](int timeout, i32* fd, i32* events, u32* data) -> int {
        int f = 0, e = 0;
        void* d = nullptr;
        int r = ALooper_pollOnce(timeout, &f, &e, &d);
        if (fd) *fd = f;
        if (events) *events = e;
        if (data) *data = (u32)(uintptr_t)d;
        return r;
    });
    reg("ALooper_addFd", +[](u32 l, int fd, int ident, int events, u32 cb, u32 data) -> int {
        auto* p = (ALooper*)ptr_host(l);
        if (cb) LOGW("ALooper_addFd with a callback is not bridged; the fd is only reported by ident");
        return p ? ALooper_addFd(p, fd, ident, events, nullptr, (void*)(uintptr_t)data) : -1;
    });
    reg("ALooper_removeFd", +[](u32 l, int fd) -> int { auto* p = (ALooper*)ptr_host(l); return p ? ALooper_removeFd(p, fd) : -1; });
    reg("ASensorManager_getInstance", +[]() -> u32 {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        return ptr_token(ASensorManager_getInstance());
#pragma clang diagnostic pop
    });
    reg("ASensorManager_getInstanceForPackage", +[](const char* pkg) -> u32 { return ptr_token(ASensorManager_getInstanceForPackage(pkg)); });
    reg("ASensorManager_getDefaultSensor", +[](u32 m, int type) -> u32 {
        auto* p = (ASensorManager*)ptr_host(m);
        return p ? ptr_token((void*)ASensorManager_getDefaultSensor(p, type)) : 0;
    });
    reg("ASensorManager_createEventQueue", +[](u32 m, u32 looper, int ident, u32 cb, u32 data) -> u32 {
        auto* p = (ASensorManager*)ptr_host(m);
        if (!p) return 0;
        if (cb) LOGW("ASensorManager_createEventQueue with a callback is not bridged; events must be read with ASensorEventQueue_getEvents");
        return ptr_token(ASensorManager_createEventQueue(p, (ALooper*)ptr_host(looper), ident, nullptr, (void*)(uintptr_t)data));
    });
    reg("ASensorManager_destroyEventQueue", +[](u32 m, u32 q) -> int {
        auto* p = (ASensorManager*)ptr_host(m);
        auto* e = (ASensorEventQueue*)ptr_host(q);
        if (!p || !e) return -1;
        int r = ASensorManager_destroyEventQueue(p, e);
        ptr_drop(q);
        return r;
    });
    reg("ASensorEventQueue_enableSensor", +[](u32 q, u32 s) -> int {
        auto* e = (ASensorEventQueue*)ptr_host(q);
        return e ? ASensorEventQueue_enableSensor(e, (const ASensor*)ptr_host(s)) : -1;
    });
    reg("ASensorEventQueue_disableSensor", +[](u32 q, u32 s) -> int {
        auto* e = (ASensorEventQueue*)ptr_host(q);
        return e ? ASensorEventQueue_disableSensor(e, (const ASensor*)ptr_host(s)) : -1;
    });
    reg("ASensorEventQueue_setEventRate", +[](u32 q, u32 s, i32 usec) -> int {
        auto* e = (ASensorEventQueue*)ptr_host(q);
        return e ? ASensorEventQueue_setEventRate(e, (const ASensor*)ptr_host(s), usec) : -1;
    });
    reg("ASensorEventQueue_hasEvents", +[](u32 q) -> int { auto* e = (ASensorEventQueue*)ptr_host(q); return e ? ASensorEventQueue_hasEvents(e) : -1; });
    // ASensorEvent has the same 104-byte layout on 32- and 64-bit ARM.
    static_assert(sizeof(ASensorEvent) == 104, "ASensorEvent layout");
    reg("ASensorEventQueue_getEvents", +[](u32 q, ASensorEvent* ev, u32 n) -> int {
        auto* e = (ASensorEventQueue*)ptr_host(q);
        return e ? (int)ASensorEventQueue_getEvents(e, ev, n) : -1;
    });
    reg("ASensor_getName", +[](u32 s) -> u32 { auto* p = (const ASensor*)ptr_host(s); return p ? guest_intern_string(ASensor_getName(p)) : 0; });
    reg("ASensor_getVendor", +[](u32 s) -> u32 { auto* p = (const ASensor*)ptr_host(s); return p ? guest_intern_string(ASensor_getVendor(p)) : 0; });
    reg("ASensor_getType", +[](u32 s) -> int { auto* p = (const ASensor*)ptr_host(s); return p ? ASensor_getType(p) : -1; });
    reg("ASensor_getResolution", +[](u32 s) -> float { auto* p = (const ASensor*)ptr_host(s); return p ? ASensor_getResolution(p) : 0.f; });
    reg("ASensor_getMinDelay", +[](u32 s) -> int { auto* p = (const ASensor*)ptr_host(s); return p ? ASensor_getMinDelay(p) : 0; });

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
    reg("eglSwapBuffers", +[](u32 d, u32 s) -> u32 {
        g_frames.fetch_add(1, std::memory_order_relaxed);
        g_render_tid.store((int)gettid(), std::memory_order_relaxed);
        static thread_local bool boosted = false;
        if (!boosted) {
            // The thread that presents frames gets display priority, like Android's own RenderThread.
            boosted = true;
            int r = setpriority(PRIO_PROCESS, 0, -10);
            if (r != 0) r = setpriority(PRIO_PROCESS, 0, -8);
            LOGI("render thread %d: priority %s", (int)gettid(), r == 0 ? "raised to display level" : "unchanged (not permitted)");
        }
        return eglSwapBuffers(ptr_host(d), ptr_host(s));
    });
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
