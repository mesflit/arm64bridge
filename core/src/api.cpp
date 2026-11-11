// Public entry points: the shim loader used inside patched apps and the JNI
// methods of the ARM64Bridge app itself.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <fcntl.h>
#include <jni.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
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

void add_guest_apk(const std::string& apk) {
    std::lock_guard<std::mutex> lk(g_provider_mu);
    if (!g_sources.insert("apk:" + apk).second) return;
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
        LOGI("ARM64Bridge %s, log file: %s", A64B_VERSION, fd >= 0 ? where.c_str() : "(none)");
        LOGI("app %s on %s %s, Android %s (API %d)", pkg.c_str(), static_string(env, "android/os/Build", "MANUFACTURER").c_str(),
             static_string(env, "android/os/Build", "MODEL").c_str(), static_string(env, "android/os/Build$VERSION", "RELEASE").c_str(),
             static_int(env, "android/os/Build$VERSION", "SDK_INT"));
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

A64B_EXPORT void Java_com_mesflit_arm64bridge_NativeBridge_addGuestDirectory(JNIEnv* env, jclass, jstring dir) {
    init_from_environment();
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
    return env->NewStringUTF("ARM64Bridge core " A64B_VERSION " (Unicorn ARMv7 engine)");
}
