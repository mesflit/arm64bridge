// Public entry points: the shim loader used inside patched apps and the JNI
// methods of the ARM64Bridge app itself.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <jni.h>
#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <set>

#include "elf_loader.h"
#include "jni_bridge.h"
#include "thunk.h"
#include "zip.h"

#define A64B_EXPORT extern "C" __attribute__((visibility("default")))

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
    std::string path = shim_path ? shim_path : "";
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
    return env->NewStringUTF("ARM64Bridge core 0.1 (Unicorn ARMv7 engine)");
}
