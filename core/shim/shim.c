/*
 * Shim library. The patcher places one copy of this file in lib/arm64-v8a/ for
 * every 32-bit library of the original app, under the original file name. When
 * the app calls System.loadLibrary("foo"), Android loads this 64-bit shim, whose
 * JNI_OnLoad asks the bridge runtime to load and run the real 32-bit libfoo.so.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <jni.h>

extern jint a64b_shim_load(JavaVM* vm, const char* shim_path);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    Dl_info info;
    (void)reserved;
    if (!dladdr((void*)&JNI_OnLoad, &info) || !info.dli_fname) return JNI_ERR;
    return a64b_shim_load(vm, info.dli_fname);
}
