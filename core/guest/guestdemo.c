/*
 * 32-bit ARM demo/test library for ARM64Bridge.
 *
 * It is compiled for armeabi-v7a only and exercised through the bridge, both by
 * the automated tests (desktop JVM) and by the "self test" button of the app.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <jni.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
typedef wchar_t wchar_t32;
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#else
#include "guest_libc.h"
#endif

#define CLS "com/mesflit/arm64bridge/GuestDemo"

static JavaVM* g_vm;
static volatile int g_ctor_ran;

__attribute__((constructor)) static void on_load_ctor(void) { g_ctor_ran = 42; }

/* ---- bound through exported Java_* symbols ---- */

JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_add(JNIEnv* env, jclass cls, jint a, jint b) {
    (void)env; (void)cls;
    return a + b;
}

JNIEXPORT jlong JNICALL Java_com_mesflit_arm64bridge_GuestDemo_mulLong(JNIEnv* env, jclass cls, jlong a, jlong b) {
    (void)env; (void)cls;
    return a * b;
}

JNIEXPORT jdouble JNICALL Java_com_mesflit_arm64bridge_GuestDemo_hypot(JNIEnv* env, jclass cls, jdouble a, jdouble b) {
    (void)env; (void)cls;
    return sqrt(a * a + b * b);
}

JNIEXPORT jfloat JNICALL Java_com_mesflit_arm64bridge_GuestDemo_halve(JNIEnv* env, jclass cls, jfloat v) {
    (void)env; (void)cls;
    return v * 0.5f;
}

/* Many arguments of mixed width: exercises register/stack marshalling on both sides. */
JNIEXPORT jdouble JNICALL Java_com_mesflit_arm64bridge_GuestDemo_mixed(JNIEnv* env, jclass cls, jint a, jlong b, jfloat c,
                                                                       jdouble d, jbyte e, jshort f, jchar g, jboolean h,
                                                                       jlong i, jint j, jdouble k, jfloat l, jint m,
                                                                       jint n, jint o, jint p, jdouble q, jdouble r,
                                                                       jdouble s, jdouble t, jdouble u, jdouble v) {
    (void)env; (void)cls;
    return a + (jdouble)b + c + d + e + f + g + (h ? 1000 : 0) + (jdouble)i + j + k + l + m + n + o + p + q + r + s + t + u + v;
}

JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_divide(JNIEnv* env, jclass cls, jint a, jint b) {
    (void)env; (void)cls;
    return a / b + a % b; /* __aeabi_idivmod on cores without hardware divide */
}

JNIEXPORT jstring JNICALL Java_com_mesflit_arm64bridge_GuestDemo_greet(JNIEnv* env, jclass cls, jstring name) {
    (void)cls;
    const char* utf = (*env)->GetStringUTFChars(env, name, NULL);
    char buf[160];
    snprintf(buf, sizeof(buf), "Hello %s from %d-bit ARM code! pi=%.4f big=%lld hex=%x", utf, (int)sizeof(void*) * 8,
             3.14159265, 1234567890123LL, 0xBEEF);
    (*env)->ReleaseStringUTFChars(env, name, utf);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_sumArray(JNIEnv* env, jclass cls, jintArray arr) {
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, arr);
    jint* e = (*env)->GetIntArrayElements(env, arr, NULL);
    jint sum = 0;
    for (jsize i = 0; i < n; i++) {
        sum += e[i];
        e[i] *= 2; /* written back on release */
    }
    (*env)->ReleaseIntArrayElements(env, arr, e, 0);
    return sum;
}

/* Calls back into Java: static int twice(int), static String describe(long, double, String) and an instance method. */
JNIEXPORT jstring JNICALL Java_com_mesflit_arm64bridge_GuestDemo_callbacks(JNIEnv* env, jobject self, jint v) {
    jclass cls = (*env)->GetObjectClass(env, self);
    jmethodID twice = (*env)->GetStaticMethodID(env, cls, "twice", "(I)I");
    jmethodID describe = (*env)->GetStaticMethodID(env, cls, "describe", "(JDLjava/lang/String;F)Ljava/lang/String;");
    jmethodID bump = (*env)->GetMethodID(env, cls, "bump", "(I)I");
    jfieldID counter = (*env)->GetFieldID(env, cls, "counter", "I");
    if (!twice || !describe || !bump || !counter) return NULL;
    jint t = (*env)->CallStaticIntMethod(env, cls, twice, v);
    jint b = (*env)->CallIntMethod(env, self, bump, t);
    jvalue args[1];
    args[0].i = 5;
    b = (*env)->CallIntMethodA(env, self, bump, args);
    (*env)->SetIntField(env, self, counter, (*env)->GetIntField(env, self, counter) + 100);
    jstring tag = (*env)->NewStringUTF(env, "tag");
    jstring out = (jstring)(*env)->CallStaticObjectMethod(env, cls, describe, (jlong)0x100000002LL, 2.5, tag, 1.5f);
    (void)b;
    return out;
}

JNIEXPORT void JNICALL Java_com_mesflit_arm64bridge_GuestDemo_throwIt(JNIEnv* env, jclass cls, jstring msg) {
    (void)cls;
    const char* utf = (*env)->GetStringUTFChars(env, msg, NULL);
    (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/IllegalStateException"), utf);
    (*env)->ReleaseStringUTFChars(env, msg, utf);
}

JNIEXPORT jlong JNICALL Java_com_mesflit_arm64bridge_GuestDemo_fib(JNIEnv* env, jclass cls, jint n) {
    (void)env; (void)cls;
    jlong a = 0, b = 1;
    for (jint i = 0; i < n; i++) {
        jlong t = a + b;
        a = b;
        b = t;
    }
    return a;
}

/* CPU-bound loop used as a rough speed indicator. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_primes(JNIEnv* env, jclass cls, jint limit) {
    (void)env; (void)cls;
    jint count = 0;
    for (jint n = 2; n < limit; n++) {
        int prime = 1;
        for (jint d = 2; d * d <= n; d++)
            if (n % d == 0) { prime = 0; break; }
        count += prime;
    }
    return count;
}

/* ---- threads, mutexes and atomics ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_locked_counter;
static volatile int g_atomic_counter;
static volatile int g_spin;            /* LDREX/STREX spin lock */
static volatile int g_spin_counter;    /* protected only by g_spin */

static void* worker(void* arg) {
    int rounds = (int)(intptr_t)arg;
    for (int i = 0; i < rounds; i++) {
        pthread_mutex_lock(&g_lock);
        g_locked_counter++;
        pthread_mutex_unlock(&g_lock);
        __sync_fetch_and_add(&g_atomic_counter, 1);
        while (__sync_lock_test_and_set(&g_spin, 1)) {
        }
        g_spin_counter = g_spin_counter + 1;
        __sync_lock_release(&g_spin);
    }
    return (void*)(intptr_t)rounds;
}

JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_threads(JNIEnv* env, jclass cls, jint nthreads, jint rounds) {
    (void)env; (void)cls;
    pthread_t t[8];
    if (nthreads > 8) nthreads = 8;
    g_locked_counter = 0;
    g_atomic_counter = 0;
    g_spin_counter = 0;
    for (int i = 0; i < nthreads; i++) pthread_create(&t[i], NULL, worker, (void*)(intptr_t)rounds);
    int joined = 0;
    for (int i = 0; i < nthreads; i++) {
        void* ret = NULL;
        pthread_join(t[i], &ret);
        joined += (int)(intptr_t)ret;
    }
    if (joined != nthreads * rounds || g_atomic_counter != nthreads * rounds || g_spin_counter != nthreads * rounds) return -1;
    return g_locked_counter;
}

/* ---- libc: qsort with a guest callback, sscanf, malloc, setjmp, NEON-friendly float math ---- */
static int cmp_int(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }
static jmp_buf g_jb;
static void jump_out(int v) { longjmp(g_jb, v); }

__attribute__((target("arm"))) static int arm_mode_helper(int x) { return x * 3 + 1; }

JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_libc(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    int score = 0;
    int* v = (int*)malloc(64 * sizeof(int));
    for (int i = 0; i < 64; i++) v[i] = (i * 37 + 11) % 64;
    qsort(v, 64, sizeof(int), cmp_int);
    int sorted = 1;
    for (int i = 0; i < 64; i++)
        if (v[i] != i) sorted = 0;
    free(v);
    score += sorted;                                   /* 1 */

    int a = 0; unsigned b = 0; char word[16]; float f = 0;
    if (sscanf("12 ff hello 2.5", "%d %x %15s %f", &a, &b, word, &f) == 4 && a == 12 && b == 255 && strcmp(word, "hello") == 0 && f == 2.5f)
        score += 2;                                    /* 3 */

    int jv = setjmp(g_jb);
    if (jv == 0) jump_out(7);
    if (jv == 7) score += 4;                           /* 7 */

    float acc = 0;
    float data[16];
    for (int i = 0; i < 16; i++) data[i] = (float)i * 0.5f;
    for (int i = 0; i < 16; i++) acc += data[i] * data[i];
    if (acc == 310.0f) score += 8;                     /* 15 */

    if (arm_mode_helper(5) == 16 && pow(2.0, 10.0) == 1024.0) score += 16; /* 31 */
    if (g_ctor_ran == 42) score += 32;                 /* 63 */
    return score;
}

/* ---- zlib, epoll, eventfd, div_t, wide strings: structures that differ between 32 and 64 bit ---- */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_posix(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    int score = 0;
    static const char text[] = "Minions! Minions! Minions! Bello! Bello! Bello! Banana banana banana banana.";
    const unsigned char* src = (const unsigned char*)text;
    unsigned long n = sizeof(text);

    if (zlibVersion() && zlibVersion()[0] == '1' && crc32(0, (const unsigned char*)"hello", 5) == 0x3610a686UL) score += 1;

    unsigned char packed[256], unpacked[256];
    unsigned long plen = sizeof(packed), ulen = sizeof(unpacked);
    if (compressBound(n) <= sizeof(packed) && compress(packed, &plen, src, n) == Z_OK && plen < n &&
        uncompress(unpacked, &ulen, packed, plen) == Z_OK && ulen == n && memcmp(unpacked, src, n) == 0)
        score += 2;

    z_stream d, i;
    memset(&d, 0, sizeof(d));
    memset(&i, 0, sizeof(i));
    unsigned char stream[256], back[256];
    int ok = deflateInit(&d, 9) == Z_OK;
    d.next_in = (unsigned char*)src; d.avail_in = (unsigned)n; d.next_out = stream; d.avail_out = sizeof(stream);
    ok = ok && deflate(&d, Z_FINISH) == Z_STREAM_END && d.total_in == n;
    unsigned slen = (unsigned)d.total_out;
    deflateEnd(&d);
    ok = ok && inflateInit2(&i, 15) == Z_OK;
    /* feed the compressed data in small pieces */
    i.next_out = back; i.avail_out = sizeof(back);
    int r = Z_OK;
    for (unsigned off = 0; ok && off < slen && r == Z_OK; off += 7) {
        i.next_in = stream + off;
        i.avail_in = slen - off < 7 ? slen - off : 7;
        r = inflate(&i, Z_NO_FLUSH);
    }
    ok = ok && r == Z_STREAM_END && i.total_out == n && memcmp(back, src, n) == 0 && i.next_out == back + n;
    inflateEnd(&i);
    if (ok) score += 4;

    int efd = eventfd(0, 0), ep = epoll_create(4);
    struct epoll_event ev, out[2];
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ULL;
    uint64_t one = 1;
    if (efd >= 0 && ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0 && write(efd, &one, 8) == 8 &&
        epoll_wait(ep, out, 2, 1000) == 1 && out[0].data.u64 == 0x1122334455667788ULL && (out[0].events & EPOLLIN))
        score += 8;
    if (efd >= 0) close(efd);
    if (ep >= 0) close(ep);

    div_t q = div(47, 5);
    if (q.quot == 9 && q.rem == 2) score += 16;

    wchar_t32 w[32];
    static const wchar_t32 wfmt[] = {'%', 'd', '-', '%', 's', 0};
    if (swprintf(w, 32, wfmt, 42, "ok") == 5 && w[0] == '4' && w[3] == 'o' && w[5] == 0 &&
        iswctype('A', wctype("upper")) && !iswctype('a', wctype("upper")))
        score += 32;
    return score;
}

/* ---- a jobject argument kept beyond the call (worked on Dalvik; old games rely on it) ---- */
static jstring g_remembered;

JNIEXPORT void JNICALL Java_com_mesflit_arm64bridge_GuestDemo_remember(JNIEnv* env, jclass cls, jstring s) {
    (void)env; (void)cls;
    g_remembered = s;
}

JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_recallLength(JNIEnv* env, jclass cls) {
    (void)cls;
    return (*env)->GetStringUTFLength(env, g_remembered);
}

/* ---- bound through RegisterNatives in JNI_OnLoad ---- */
static jstring native_registered(JNIEnv* env, jobject self, jint x, jstring s) {
    (void)self;
    const char* utf = (*env)->GetStringUTFChars(env, s, NULL);
    char buf[96];
    snprintf(buf, sizeof(buf), "registered:%d:%s", x * 2, utf);
    (*env)->ReleaseStringUTFChars(env, s, utf);
    return (*env)->NewStringUTF(env, buf);
}

static jboolean native_attached(JNIEnv* env, jclass cls) {
    (void)cls;
    /* The JavaVM handed to JNI_OnLoad must give back this thread's env. */
    JNIEnv* e2 = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void**)&e2, JNI_VERSION_1_6) != JNI_OK) return JNI_FALSE;
    return e2 == env ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env = NULL;
    g_vm = vm;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = (*env)->FindClass(env, CLS);
    if (!cls) return JNI_ERR;
    static const JNINativeMethod methods[] = {
        {"registered", "(ILjava/lang/String;)Ljava/lang/String;", (void*)native_registered},
        {"sameEnv", "()Z", (void*)native_attached},
    };
    if ((*env)->RegisterNatives(env, cls, methods, 2) != JNI_OK) return JNI_ERR;
    __android_log_print(4, "guestdemo", "JNI_OnLoad done, pointer size %d", (int)sizeof(void*));
    return JNI_VERSION_1_6;
}
