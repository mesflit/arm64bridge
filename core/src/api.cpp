// Public entry points: the shim loader used inside patched apps and the JNI
// methods of the ARM64Bridge app itself.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <fcntl.h>
#include <jni.h>
#include <limits.h>
#include <time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <set>

#include "elf_loader.h"
#include "jni_bridge.h"
#include "thunk.h"
#include "zip.h"

#define A64B_EXPORT extern "C" __attribute__((visibility("default")))
#ifndef A64B_VERSION
#define A64B_VERSION "dev"
#endif

namespace a64b {
namespace {

std::mutex g_provider_mu;
std::set<std::string> g_sources;

bool read_file(const std::string& path, std::vector<u8>& out) {
    FILE* f = fopen(path.c_str(), "rbe");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

void add_guest_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_provider_mu);
    if (!g_sources.insert("dir:" + dir).second) return;
    LOGI("guest library directory: %s", dir.c_str());
    loader_add_provider([dir](const std::string& name, std::vector<u8>& out) { return read_file(dir + "/" + name, out); });
}

std::string g_apk_path;

void add_guest_apk(const std::string& apk) {
    std::lock_guard<std::mutex> lk(g_provider_mu);
    if (!g_sources.insert("apk:" + apk).second) return;
    if (g_apk_path.empty()) g_apk_path = apk;
    LOGI("guest libraries from APK: %s", apk.c_str());
    loader_add_provider([apk](const std::string& name, std::vector<u8>& out) {
        return zip_extract(apk, "lib/armeabi-v7a/" + name, out) || zip_extract(apk, "lib/armeabi/" + name, out);
    });
}

void init_from_environment() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* v = getenv("A64B_LOG"))
            if (*v == 'd' || *v == 'D') g_log_level = LOG_DEBUG;
        if (const char* d = getenv("A64B_GUEST_DIR")) add_guest_dir(d);
    });
}

bool clear_exc(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

std::string jstr(JNIEnv* env, jstring s);
long long thread_cpu_ticks(int tid);
int thread_cpu_core(int tid);

jobject current_application(JNIEnv* env) {
    jclass at = env->FindClass("android/app/ActivityThread");
    if (clear_exc(env) || !at) return nullptr;
    jmethodID cur = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
    jobject app = cur ? env->CallStaticObjectMethod(at, cur) : nullptr;
    if (clear_exc(env)) app = nullptr;
    env->DeleteLocalRef(at);
    return app;
}

int static_int(JNIEnv* env, const char* cls_name, const char* field) {
    jclass cls = env->FindClass(cls_name);
    if (clear_exc(env) || !cls) return 0;
    jfieldID f = env->GetStaticFieldID(cls, field, "I");
    int v = f ? env->GetStaticIntField(cls, f) : 0;
    clear_exc(env);
    env->DeleteLocalRef(cls);
    return v;
}

std::string static_string(JNIEnv* env, const char* cls_name, const char* field) {
    jclass cls = env->FindClass(cls_name);
    if (clear_exc(env) || !cls) return "";
    jfieldID f = env->GetStaticFieldID(cls, field, "Ljava/lang/String;");
    jstring v = f ? (jstring)env->GetStaticObjectField(cls, f) : nullptr;
    clear_exc(env);
    std::string r = jstr(env, v);
    env->DeleteLocalRef(cls);
    return r;
}

// Creates Download/ARM64Bridge/<name> through MediaStore (Android 10+, no permission needed)
// and returns a raw file descriptor for it, or -1.
int open_download_file(JNIEnv* env, jobject app, const std::string& name) {
    if (static_int(env, "android/os/Build$VERSION", "SDK_INT") < 29) return -1;
    int fd = -1;
    jclass ctx_cls = env->GetObjectClass(app);
    jmethodID get_cr = env->GetMethodID(ctx_cls, "getContentResolver", "()Landroid/content/ContentResolver;");
    jobject cr = env->CallObjectMethod(app, get_cr);
    jclass cv_cls = env->FindClass("android/content/ContentValues");
    jclass dl_cls = env->FindClass("android/provider/MediaStore$Downloads");
    if (clear_exc(env) || !cr || !cv_cls || !dl_cls) return -1;
    jobject cv = env->NewObject(cv_cls, env->GetMethodID(cv_cls, "<init>", "()V"));
    jmethodID put = env->GetMethodID(cv_cls, "put", "(Ljava/lang/String;Ljava/lang/String;)V");
    auto put_str = [&](const char* k, const char* v) {
        jstring jk = env->NewStringUTF(k), jv = env->NewStringUTF(v);
        env->CallVoidMethod(cv, put, jk, jv);
        env->DeleteLocalRef(jk);
        env->DeleteLocalRef(jv);
    };
    put_str("_display_name", name.c_str());
    put_str("mime_type", "text/plain");
    put_str("relative_path", "Download/ARM64Bridge");
    jfieldID uri_f = env->GetStaticFieldID(dl_cls, "EXTERNAL_CONTENT_URI", "Landroid/net/Uri;");
    jobject base = uri_f ? env->GetStaticObjectField(dl_cls, uri_f) : nullptr;
    jclass cr_cls = env->GetObjectClass(cr);
    jmethodID insert = env->GetMethodID(cr_cls, "insert", "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;");
    jobject uri = (base && insert) ? env->CallObjectMethod(cr, insert, base, cv) : nullptr;
    if (!clear_exc(env) && uri) {
        jmethodID open_fd = env->GetMethodID(cr_cls, "openFileDescriptor", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
        jstring mode = env->NewStringUTF("w");
        jobject pfd = env->CallObjectMethod(cr, open_fd, uri, mode);
        if (!clear_exc(env) && pfd) {
            jclass pfd_cls = env->GetObjectClass(pfd);
            fd = env->CallIntMethod(pfd, env->GetMethodID(pfd_cls, "detachFd", "()I"));
            if (clear_exc(env)) fd = -1;
        }
    }
    clear_exc(env);
    return fd;
}

// Every patched app writes its bridge log to Download/ARM64Bridge/ (or, before Android 10,
// to Android/data/<package>/files/) so that problems can be reported without a computer.
void open_log_file(JNIEnv* env) {
    static std::once_flag once;
    std::call_once(once, [env] {
        jobject app = current_application(env);
        if (!app) return;
        jclass ctx_cls = env->GetObjectClass(app);
        std::string pkg = jstr(env, (jstring)env->CallObjectMethod(app, env->GetMethodID(ctx_cls, "getPackageName", "()Ljava/lang/String;")));
        clear_exc(env);
        jobject loader = env->CallObjectMethod(app, env->GetMethodID(ctx_cls, "getClassLoader", "()Ljava/lang/ClassLoader;"));
        if (!clear_exc(env)) jni_set_app_class_loader(env, loader);

        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        std::string name = "arm64bridge-" + pkg + "-" + stamp + ".log";
        int fd = open_download_file(env, app, name);
        std::string where = "Download/ARM64Bridge/" + name;
        if (fd < 0) {
            jobject dir = env->CallObjectMethod(app, env->GetMethodID(ctx_cls, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;"), nullptr);
            if (!clear_exc(env) && dir) {
                jclass file_cls = env->GetObjectClass(dir);
                std::string path = jstr(env, (jstring)env->CallObjectMethod(dir, env->GetMethodID(file_cls, "getAbsolutePath", "()Ljava/lang/String;")));
                clear_exc(env);
                where = path + "/" + name;
                fd = open(where.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            }
        }
        if (fd >= 0) log_set_file(fd);
        // Every 5 s: frames drawn, guest->host calls and Java->native calls. Shows whether a
        // "frozen" game still renders and whether touch events reach the native code.
        std::thread([] {
            u64 last_frames = 0, last_calls = 0;
            int last_tid = 0;
            long long last_ticks = 0, last_proc = 0;
            const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                u64 f = g_frames.load(), k = cpu_total_host_calls();
                if (f == 0) f = jni_frame_method_calls();  // frames presented from Java
                // How busy the render thread and the whole process are: a render thread at ~100%
                // means the frame rate is limited by how fast guest code is emulated.
                int tid = g_render_tid.load();
                long long ticks = tid ? thread_cpu_ticks(tid) : -1;
                long long proc = 0;
                {
                    FILE* pf = fopen("/proc/self/stat", "re");
                    if (pf) {
                        char b[1024];
                        size_t n = fread(b, 1, sizeof(b) - 1, pf);
                        b[n] = 0;
                        fclose(pf);
                        unsigned long long ut = 0, st = 0;
                        const char* q = strrchr(b, ')');
                        if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) proc = (long long)(ut + st);
                    }
                }
                int render_pct = (tid && tid == last_tid && ticks >= 0) ? (int)((ticks - last_ticks) * 100 / (5 * hz)) : -1;
                int proc_pct = last_proc ? (int)((proc - last_proc) * 100 / (5 * hz)) : -1;
                LOGI("heartbeat: %.1f fps, render thread CPU %d%% (core %d), process CPU %d%%, %llu guest->host calls/s, %d guest threads; Java->native: %s",
                     (f - last_frames) / 5.0, render_pct, tid ? thread_cpu_core(tid) : -1, proc_pct, (unsigned long long)(k >= last_calls ? (k - last_calls) / 5 : 0),  // the total drops when threads end
                     g_guest_threads.load(), jni_call_stats().c_str());
                if (tid) {
                    std::string prof = cpu_profile_report(tid, 5.0);
                    if (!prof.empty()) LOGI("profile: %s", prof.c_str());
                }
                last_tid = tid;
                last_ticks = ticks;
                last_proc = proc;
                last_frames = f;
                last_calls = k;
            }
        }).detach();
        LOGI("ARM64Bridge %s, log file: %s", A64B_VERSION, fd >= 0 ? where.c_str() : "(none)");
        LOGI("app %s on %s %s, Android %s (API %d)", pkg.c_str(), static_string(env, "android/os/Build", "MANUFACTURER").c_str(),
             static_string(env, "android/os/Build", "MODEL").c_str(), static_string(env, "android/os/Build$VERSION", "RELEASE").c_str(),
             static_int(env, "android/os/Build$VERSION", "SDK_INT"));
    });
}

// CPU core a thread last ran on (field 39 of /proc/<pid>/task/<tid>/stat), or -1.
int thread_cpu_core(int tid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    FILE* f = fopen(path, "re");
    if (!f) return -1;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* p = strrchr(buf, ')');
    if (!p) return -1;
    int field = 2;  // ")" ends field 2
    for (const char* q = p + 1; *q; q++) {
        if (*q != ' ') continue;
        field++;
        if (field == 39) return atoi(q + 1);
    }
    return -1;
}

// CPU time (user + system, in clock ticks) a thread of this process has used so far.
long long thread_cpu_ticks(int tid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    FILE* f = fopen(path, "re");
    if (!f) return -1;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    const char* p = strrchr(buf, ')');  // the thread name may contain spaces
    if (!p) return -1;
    unsigned long long ut = 0, st = 0;
    // fields after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return -1;
    return (long long)(ut + st);
}

// ---- in-game menu (com.mesflit.arm64bridge.overlay.GameOverlay, loaded from the patched APK) ----
void JNICALL overlay_set_limiter(JNIEnv*, jclass, jint mode) {
    g_frame_limiter.store(mode < 0 || mode > 2 ? 0 : mode);
    LOGI("in-game menu: frame limiter mode %d", (int)mode);
}

jstring JNICALL overlay_version(JNIEnv* env, jclass) { return env->NewStringUTF(A64B_VERSION); }

// Value editor: memory of this process, read and written with process_vm_readv/writev so that a bad
// address is an error and not a crash. (Opening /proc/self/mem is only allowed in debuggable apps.)
jint overlay_mem(JNIEnv* env, jlong address, jbyteArray array, jint length, bool write) {
    if (!array || length <= 0 || length > env->GetArrayLength(array)) return -EINVAL;
    jbyte* bytes = env->GetByteArrayElements(array, nullptr);
    if (!bytes) return -ENOMEM;
    iovec local = {bytes, (size_t)length};
    iovec remote = {(void*)(uintptr_t)address, (size_t)length};
    ssize_t n = write ? process_vm_writev(getpid(), &local, 1, &remote, 1, 0) : process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    int result = n < 0 ? -errno : (int)n;
    env->ReleaseByteArrayElements(array, bytes, write ? JNI_ABORT : 0);
    return result;
}
jint JNICALL overlay_mem_read(JNIEnv* env, jclass, jlong address, jbyteArray dst, jint length) {
    return overlay_mem(env, address, dst, length, false);
}
jint JNICALL overlay_mem_write(JNIEnv* env, jclass, jlong address, jbyteArray src, jint length) {
    return overlay_mem(env, address, src, length, true);
}

jfloat JNICALL overlay_fps(JNIEnv*, jclass) {
    static u64 last_frames = 0;
    static auto last_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    u64 frames = g_frames.load();
    if (frames == 0) frames = jni_frame_method_calls();  // frames presented from Java
    double dt = std::chrono::duration<double>(now - last_time).count();
    float fps = (frames == 0 || dt <= 0) ? -1.0f : (float)((frames - last_frames) / dt);
    last_frames = frames;
    last_time = now;
    return fps;
}

// The patcher puts the dex of the ARM64Bridge app into assets/arm64bridge/ (FPS display and in-game menu).
void load_overlay(JNIEnv* env) {
    static std::once_flag once;
    std::call_once(once, [env] {
        if (g_apk_path.empty()) return;
        static std::vector<std::vector<u8>*> dexes;  // must stay alive: the class loader maps them
        for (int i = 1; i <= 16; i++) {
            auto* data = new std::vector<u8>();
            std::string name = i == 1 ? "classes.dex" : "classes" + std::to_string(i) + ".dex";
            if (!zip_extract(g_apk_path, "assets/arm64bridge/" + name, *data)) {
                delete data;
                break;
            }
            dexes.push_back(data);
        }
        if (dexes.empty()) return;
        jobject app = current_application(env);
        jclass bb_cls = env->FindClass("java/nio/ByteBuffer");
        jclass cl_cls = env->FindClass("java/lang/ClassLoader");
        jclass im_cls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (clear_exc(env) || !app || !bb_cls || !cl_cls || !im_cls) {
            LOGW("in-game menu: not available on this Android version");
            return;
        }
        jobjectArray buffers = env->NewObjectArray((jsize)dexes.size(), bb_cls, nullptr);
        for (size_t i = 0; i < dexes.size(); i++) {
            jobject b = env->NewDirectByteBuffer(dexes[i]->data(), (jlong)dexes[i]->size());
            env->SetObjectArrayElement(buffers, (jsize)i, b);
            env->DeleteLocalRef(b);
        }
        jobject parent = env->CallStaticObjectMethod(cl_cls, env->GetStaticMethodID(cl_cls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;"));
        jmethodID ctor = env->GetMethodID(im_cls, "<init>", "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        jobject loader = (!clear_exc(env) && ctor) ? env->NewObject(im_cls, ctor, buffers, parent) : nullptr;
        if (clear_exc(env) || !loader) {
            LOGW("in-game menu: could not load the menu code");
            return;
        }
        jstring cname = env->NewStringUTF("com.mesflit.arm64bridge.overlay.GameOverlay");
        jclass overlay = (jclass)env->CallObjectMethod(loader, env->GetMethodID(cl_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), cname);
        if (clear_exc(env) || !overlay) {
            LOGW("in-game menu: menu class not found");
            return;
        }
        env->NewGlobalRef(loader);
        overlay = (jclass)env->NewGlobalRef(overlay);
        const JNINativeMethod natives[] = {{const_cast<char*>("nativeSetLimiter"), const_cast<char*>("(I)V"), (void*)overlay_set_limiter},
                                           {const_cast<char*>("nativeFps"), const_cast<char*>("()F"), (void*)overlay_fps},
                                           {const_cast<char*>("nativeVersion"), const_cast<char*>("()Ljava/lang/String;"), (void*)overlay_version},
                                           {const_cast<char*>("nativeMemRead"), const_cast<char*>("(J[BI)I"), (void*)overlay_mem_read},
                                           {const_cast<char*>("nativeMemWrite"), const_cast<char*>("(J[BI)I"), (void*)overlay_mem_write}};
        // The FPS booster controls are an option of the patcher (marker file in the APK).
        std::vector<u8> marker;
        bool booster = zip_extract(g_apk_path, "assets/arm64bridge/fps-booster", marker);
        if (!booster) g_frame_limiter.store(0);
        bool editor = zip_extract(g_apk_path, "assets/arm64bridge/value-editor", marker);
        jmethodID install = env->GetStaticMethodID(overlay, "install", "(Landroid/app/Application;ZZ)V");
        if (env->RegisterNatives(overlay, natives, 5) != JNI_OK || clear_exc(env) || !install) {
            clear_exc(env);
            LOGW("in-game menu: could not bind the menu");
            return;
        }
        env->CallStaticVoidMethod(overlay, install, app, (jboolean)booster, (jboolean)editor);
        if (clear_exc(env)) LOGW("in-game menu: installation failed");
        else LOGI("in-game menu installed (FPS booster %s, value editor %s)", booster ? "on" : "off", editor ? "on" : "off");
    });
}

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

}  // namespace
}  // namespace a64b

using namespace a64b;

// Called from the JNI_OnLoad of a shim library (lib/arm64-v8a/libfoo.so inside a
// patched APK). shim_path tells us which 32-bit library it stands in for.
A64B_EXPORT jint a64b_shim_load(JavaVM* vm, const char* shim_path) {
    init_from_environment();
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_ERR;
    open_log_file(env);
    crash_handler_install();
    java_proc_hook_install();
    std::string path = shim_path ? shim_path : "";
    LOGI("System.loadLibrary -> shim %s", path.c_str());
    std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);

    // Locate the APK that carries the 32-bit libraries.
    size_t bang = path.find("!/");
    if (bang != std::string::npos) {
        add_guest_apk(path.substr(0, bang));  // library is mapped straight from the APK
    } else {
        char real[PATH_MAX];
        std::string dir = path.substr(0, path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
        // <app dir>/lib/arm64/libfoo.so -> <app dir>/base.apk
        std::string candidate = dir + "/../../base.apk";
        if (realpath(candidate.c_str(), real) && access(real, R_OK) == 0) add_guest_apk(real);
    }
    load_overlay(env);
    std::string error;
    jint v = jni_load_library(env, name, &error);
    LOGI("%s: load finished (%s)", name.c_str(), v == JNI_ERR ? "FAILED" : "ok");
    if (v == JNI_ERR) {
        LOGE("ARM64Bridge could not load %s: %s", name.c_str(), error.c_str());
        jclass ule = env->FindClass("java/lang/UnsatisfiedLinkError");
        if (ule) env->ThrowNew(ule, ("ARM64Bridge: " + name + ": " + error).c_str());
        return JNI_VERSION_1_6;  // the pending exception reports the failure
    }
    return v >= JNI_VERSION_1_1 ? v : JNI_VERSION_1_6;
}

// ----- com.mesflit.arm64bridge.NativeBridge -----

A64B_EXPORT jstring Java_com_mesflit_arm64bridge_NativeBridge_inspectElf(JNIEnv* env, jclass, jbyteArray data) {
    jsize n = env->GetArrayLength(data);
    std::vector<u8> buf((size_t)n);
    env->GetByteArrayRegion(data, 0, n, (jbyte*)buf.data());
    ElfInfo info = elf_inspect(buf.data(), buf.size());
    std::string out;
    out += "valid=" + std::string(info.valid ? "1" : "0") + "\n";
    out += "arm32=" + std::string(info.arm32 ? "1" : "0") + "\n";
    out += "machine=" + info.machine + "\n";
    if (!info.error.empty()) out += "error=" + info.error + "\n";
    for (auto& s : info.needed) out += "needed=" + s + "\n";
    out += "imports=" + std::to_string(info.imports.size()) + "\n";
    out += "exports=" + std::to_string(info.exports.size()) + "\n";
    // Imports are only "missing" if neither the bridge nor (possibly) another bundled library has them;
    // the caller subtracts the exports of the sibling libraries.
    for (auto& s : info.imports)
        if (!thunk_exists(s)) out += "unresolved=" + s + "\n";
    for (auto& s : info.exports) out += "export=" + s + "\n";
    return env->NewStringUTF(out.c_str());
}

// Called by the BridgeStarter provider of a patched app, before the app's own code runs.
A64B_EXPORT void Java_com_mesflit_arm64bridge_overlay_BridgeStarter_nativeStart(JNIEnv*, jclass) {
    init_from_environment();
    java_proc_hook_install();
}

A64B_EXPORT void Java_com_mesflit_arm64bridge_NativeBridge_addGuestDirectory(JNIEnv* env, jclass, jstring dir) {
    init_from_environment();
    java_proc_hook_install();  // for the self test
    add_guest_dir(jstr(env, dir));
}

A64B_EXPORT jstring Java_com_mesflit_arm64bridge_NativeBridge_loadGuestLibrary(JNIEnv* env, jclass, jstring name) {
    init_from_environment();
    std::string error;
    jint v = jni_load_library(env, jstr(env, name), &error);
    if (v == JNI_ERR) return env->NewStringUTF(error.empty() ? "load failed" : error.c_str());
    return nullptr;
}

A64B_EXPORT jstring Java_com_mesflit_arm64bridge_NativeBridge_version(JNIEnv* env, jclass) {
    return env->NewStringUTF("ARM64Bridge core " A64B_VERSION " (dynarmic ARMv7 recompiler)");
}
