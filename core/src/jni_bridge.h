// Bridges JNI between the 64-bit host runtime and 32-bit guest libraries.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <jni.h>

#include <string>

#include "common.h"
#include "cpu.h"
#include "elf_loader.h"

namespace a64b {

bool jni_bridge_init(JavaVM* vm);
JavaVM* jni_host_vm();
// Host JNIEnv of the calling thread (attaches lazily through GetEnv).
JNIEnv* jni_host_env(Cpu& c);
u32 jni_new_local(Cpu& c, jobject obj);
jobject jni_to_host(Cpu& c, u32 token);

// Scope of one Java -> guest transition: sets the host env and releases the
// local reference tokens created inside it.
struct JniFrame {
    Cpu& c;
    size_t base;
    void* saved_env;
    JniFrame(Cpu& cpu, JNIEnv* env);
    ~JniFrame();
};

// Registers every exported Java_* function of the module with its Java class.
int jni_bind_java_exports(JNIEnv* env, Module* m);
// Calls the guest JNI_OnLoad (if present). Returns the JNI version to report.
jint jni_call_onload(JNIEnv* env, Module* m);
// Loads a guest library for Java code: load, bind natives, JNI_OnLoad.
jint jni_load_library(JNIEnv* env, const std::string& name, std::string* error);

// Host entry point for a guest native method (see trampoline_*.S).
void* jni_make_trampoline(u32 guest_fn, const std::string& shorty, const std::string& name);
std::string jni_shorty_from_signature(const char* sig);
// Class loader used to look up the app's classes without running their static initialisers.
void jni_set_app_class_loader(JNIEnv* env, jobject loader);
// Java -> guest native calls per method since the previous call of this function.
std::string jni_call_stats();

}  // namespace a64b
