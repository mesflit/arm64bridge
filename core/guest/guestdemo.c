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
#include <sys/mman.h>
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

/* The start of the Gameloft engine's frame limiter (same instructions), for the FPS booster test:
   reads the clock with gettimeofday and returns the s64 frame time stored at obj+8. */
__asm__(
    ".text\n.arm\n.align 2\n"
    ".type test_frame_limiter, %function\n"
    "test_frame_limiter:\n"
    "  push {r4, r5, r6, r7, r8, r9, r10, r11, lr}\n"
    "  sub sp, sp, #20\n"
    "  mov r4, r0\n"
    "  add r0, sp, #12\n"
    "  mov r1, #0\n"
    "  bl gettimeofday\n"
    "  ldr r2, [sp, #16]\n"
    "  movw r7, #0x4240\n"
    "  ldr r0, [sp, #12]\n"
    "  movt r7, #0xf\n"
    "  str r4, [sp, #8]\n"
    "  asr r3, r2, #31\n"
    "  ldr r6, [r4, #16]!\n"
    "  smlal r2, r3, r0, r7\n"
    "  ldrd r0, r1, [r4, #-8]\n"
    "  add sp, sp, #20\n"
    "  pop {r4, r5, r6, r7, r8, r9, r10, r11, pc}\n"
    ".size test_frame_limiter, .-test_frame_limiter\n");
long long test_frame_limiter(void* obj);

/* The Gameloft game loop frame sleep (same instructions from "mov r2, r0" to "bl usleep"):
   r0:r1 = now (ms), r2 = &last, r3 = where "now" is stored. Sleeps last + 30 - now ms when that is
   1..29; returns 1 if it called usleep. */
__asm__(
    ".text\n.arm\n.align 2\n"
    ".type test_frame_sleep, %function\n"
    "test_frame_sleep:\n"
    "  push {r4, r5, r11, lr}\n"
    "  mov r5, r2\n"
    "  sub r4, r3, pc\n"           /* r4 = r3 - (here + 8) */
    "  sub r4, r4, #20\n"          /* the add below is 20 bytes further */
    "  mov r2, r0\n"
    "  mov r3, r1\n"
    "  ldrd r0, r1, [r5]\n"
    "  add r5, pc, r4\n"           /* r5 = original r3 */
    "  orrs r1, r0, r1\n"
    "  strd r2, r3, [r5]\n"
    "  .word 0x0a000011\n"         /* beq skip (19 instructions further) */
    "  rsb r1, r2, #30\n"
    "  add r0, r1, r0\n"
    "  sub r1, r0, #1\n"
    "  cmp r1, #28\n"
    "  .word 0x8a000002\n"         /* bhi after the call */
    "  mov r1, #1000\n"
    "  mul r0, r0, r1\n"
    "  bl usleep\n"
    "  mov r0, #1\n"
    "  b 2f\n"
    "  .rept 8\n  nop\n  .endr\n"
    "  mov r0, #0\n"               /* skip: no previous frame */
    "2:\n"
    "  pop {r4, r5, r11, pc}\n"
    ".size test_frame_sleep, .-test_frame_sleep\n");
int test_frame_sleep(unsigned lo, unsigned hi, long long* last, long long* store);

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

/* Runs the frame limiter code on an object whose frame time is `target` us; returns what the code read. */
JNIEXPORT jlong JNICALL Java_com_mesflit_arm64bridge_GuestDemo_frameLimiter(JNIEnv* env, jclass cls, jlong target) {
    struct { unsigned char enabled; unsigned char pad[7]; long long frame_us; long long last_us; } obj = {1, {0}, target, 0};
    return test_frame_limiter(&obj);
}

/* Runs the game loop frame sleep as if the previous frame started elapsed_ms ago; returns 1 if it slept. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_frameSleep(JNIEnv* env, jclass cls, jint elapsed_ms) {
    long long now = 1000000, last = now - elapsed_ms, store = 0;
    return test_frame_sleep((unsigned)now, 0, &last, &store);
}

/* ARM Linux kernel user helpers at their fixed addresses; returns 0 when all of them work. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_kuser(JNIEnv* env, jclass cls) {
    typedef int (*cmpxchg_t)(int oldval, int newval, volatile int* ptr);
    typedef int (*cmpxchg64_t)(const long long* oldval, const long long* newval, volatile long long* ptr);
    typedef void (*barrier_t)(void);
    typedef void* (*get_tls_t)(void);
    cmpxchg_t cmpxchg = (cmpxchg_t)0xffff0fc0;
    cmpxchg64_t cmpxchg64 = (cmpxchg64_t)0xffff0f60;
    int failed = 0;
    volatile int v = 5;
    if (cmpxchg(5, 7, &v) != 0 || v != 7) failed |= 1;
    if (cmpxchg(5, 9, &v) == 0 || v != 7) failed |= 2;
    ((barrier_t)0xffff0fa0)();
    volatile long long w = 0x100000002LL;
    long long o = 0x100000002LL, n = 0x300000004LL, bad = 1;
    if (cmpxchg64(&o, &n, &w) != 0 || w != n) failed |= 4;
    if (cmpxchg64(&bad, &o, &w) == 0 || w != n) failed |= 8;
    if (((get_tls_t)0xffff0fe0)() != __builtin_thread_pointer()) failed |= 16;
    return failed;
}

#ifdef __ANDROID__  /* built with the NDK's headers (APK) */
#include <sys/mman.h>
#else              /* built with the minimal headers of the desktop test */
void* mmap(void* addr, unsigned len, int prot, int flags, int fd, long offset);
int munmap(void* addr, unsigned len);
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void*)-1)
#endif

/* mmap at requested addresses, as a library loader inside a game does; returns 0 when everything works. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_mmapFixed(JNIEnv* env, jclass cls) {
    int failed = 0;
    /* Reserve a range, then map pages at exact addresses inside it. */
    char* r = mmap(NULL, 16 * 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) return 1;
    r[5 * 4096] = 42;
    char* q = mmap(r + 4 * 4096, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (q != r + 4 * 4096) failed |= 2;
    else if (q[4096] != 0) failed |= 4; /* a fixed mapping replaces the old pages */
    else { q[0] = 7; if (r[4 * 4096] != 7) failed |= 8; }
    /* MAP_FIXED that runs past the end of an earlier mapping into free memory (a loader extending
       the last segment of a library). */
    char* tail = mmap(r + 14 * 4096, 6 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (tail != r + 14 * 4096) failed |= 128;
    else { tail[5 * 4096] = 9; if (tail[5 * 4096] != 9) failed |= 256; munmap(tail, 6 * 4096); }
    munmap(r, 16 * 4096);
    /* A hint for free memory is honoured: the range just released. */
    char* h = mmap(r, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (h != r) failed |= 16;
    if (h != MAP_FAILED) munmap(h, 4096);
    /* An address outside the bridge's own arena works when the host has nothing there. */
    volatile int* x = mmap((void*)0x23450000, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (x != MAP_FAILED) {
        if ((unsigned)x != 0x23450000u) failed |= 32;
        else { x[1000] = 123456; if (x[1000] != 123456) failed |= 64; munmap((void*)x, 2 * 4096); }
    }
    return failed;
}

FILE* fopen(const char* path, const char* mode);
int fileno(FILE* f);
int fputs(const char* s, FILE* f);
int fflush(FILE* f);
int fclose(FILE* f);
int fsync(int fd);
int usleep(unsigned us);

/* Old bionic headers read the descriptor from the FILE structure; a thread joining itself must not fail. */
static void* join_self(void* arg) {
    pthread_t* self = arg;
    return (void*)(long)pthread_join(*self, NULL);
}
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_compat(JNIEnv* env, jclass cls, jstring path) {
    int failed = 0;
    const char* p = (*env)->GetStringUTFChars(env, path, NULL);
    FILE* f = fopen(p, "w");
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (!f) return 1;
    short fd_in_struct = ((short*)f)[7]; /* _file at offset 14 */
    if (fd_in_struct != fileno(f) || fd_in_struct < 3) failed |= 2;
    fputs("x", f);
    fflush(f);
    if (fsync(fd_in_struct) != 0) failed |= 4;
    fclose(f);
    pthread_t t;
    if (pthread_create(&t, NULL, join_self, &t) != 0) return failed | 8;
    usleep(200000); /* the thread joins itself and ends */
    return failed;
}

/* Old-style dlopen handle: a soinfo with an ELF hash table, read the way the Android 2.x linker does.
   Returns 0 when a symbol found that way is the function dlsym returns, and it works. */
void* dlopen(const char* name, int flags);
void* dlsym(void* handle, const char* name);
static unsigned old_elf_hash(const char* name) {
    unsigned h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_soinfo(JNIEnv* env, jclass cls) {
    struct sym { unsigned name, value, size; unsigned char info, other; unsigned short shndx; };
    char* si = dlopen("libc.so", 0);
    if (!si) return 1;
    if (si[0] != 'l' || si[3] != 'c') return 2;                  /* name */
    unsigned base = *(unsigned*)(si + 0x8c);
    const char* strtab = *(const char**)(si + 0xac);
    struct sym* symtab = *(struct sym**)(si + 0xb0);
    unsigned nbucket = *(unsigned*)(si + 0xb4);
    unsigned* bucket = *(unsigned**)(si + 0xbc);
    unsigned* chain = *(unsigned**)(si + 0xc0);
    const char* wanted[] = {"strlen", "malloc", "pthread_create", "__errno", "does_not_exist"};
    for (int w = 0; w < 5; w++) {
        void* found = 0;
        for (unsigned n = bucket[old_elf_hash(wanted[w]) % nbucket]; n; n = chain[n]) {
            const char* a = strtab + symtab[n].name;
            const char* b = wanted[w];
            while (*a && *a == *b) { a++; b++; }
            if (*a || *b) continue;
            if ((symtab[n].info >> 4) != 1 || symtab[n].shndx == 0) continue;
            found = (void*)(symtab[n].value + base);
            break;
        }
        if (w == 4) { if (found) return 8; continue; }
        if (!found || found != dlsym(si, wanted[w])) return 16 + w;
        if (w == 0 && ((unsigned (*)(const char*))found)("hello") != 5) return 4;
    }
    return 0;
}

/* A library mapped by the app's own linker imports a function the bridge lacks: after dlopen of a
   system library, the name is in the old-style symbol table and its stub returns 0. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_foreignImports(JNIEnv* env, jclass cls) {
    unsigned char* img = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (img == MAP_FAILED) return 1;
    unsigned* w;
    /* ELF header */
    img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F'; img[4] = 1; img[5] = 1; img[6] = 1;
    *(unsigned short*)(img + 16) = 3;   /* ET_DYN */
    *(unsigned short*)(img + 18) = 40;  /* EM_ARM */
    *(unsigned*)(img + 28) = 52;        /* phoff */
    *(unsigned short*)(img + 42) = 32;
    *(unsigned short*)(img + 44) = 2;   /* phnum */
    w = (unsigned*)(img + 52);          /* PT_LOAD at 0, PT_DYNAMIC at 0x200 */
    w[0] = 1; w[1] = 0; w[2] = 0; w[3] = 0; w[4] = 0x2000; w[5] = 0x2000; w[6] = 6; w[7] = 0x1000;
    w[8] = 2; w[9] = 0x200; w[10] = 0x200; w[11] = 0x200; w[12] = 64; w[13] = 64; w[14] = 6; w[15] = 4;
    /* strtab at 0x400, symtab at 0x500 (2 symbols), hash at 0x600 */
    const char name[] = "a64b_test_missing_function";
    for (unsigned i = 0; i < sizeof(name); i++) img[0x401 + i] = name[i];
    w = (unsigned*)(img + 0x510);
    w[0] = 1; w[1] = 0; w[2] = 0; img[0x510 + 12] = (1 << 4) | 2; /* GLOBAL FUNC, undefined */
    w = (unsigned*)(img + 0x600); w[0] = 1; w[1] = 2;
    w = (unsigned*)(img + 0x200);
    w[0] = 4; w[1] = 0x600; w[2] = 5; w[3] = 0x400; w[4] = 6; w[5] = 0x500; w[6] = 0; w[7] = 0;

    struct sym { unsigned name, value, size; unsigned char info, other; unsigned short shndx; };
    char* si = dlopen("libm.so", 0);
    if (!si) return 2;
    const char* strtab = *(const char**)(si + 0xac);
    struct sym* symtab = *(struct sym**)(si + 0xb0);
    unsigned nbucket = *(unsigned*)(si + 0xb4);
    unsigned* bucket = *(unsigned**)(si + 0xbc);
    unsigned* chain = *(unsigned**)(si + 0xc0);
    void* found = 0;
    for (unsigned n = bucket[old_elf_hash(name) % nbucket]; n; n = chain[n]) {
        const char* a = strtab + symtab[n].name;
        const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) { found = (void*)symtab[n].value; break; }
    }
    munmap(img, 2 * 4096);
    if (!found) return 4;
    if (((int (*)(void))found)() != 0) return 8;
    return 0;
}

/* The handle of an app library (this one) read the old way: its own exports are in the table. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_soinfoModule(JNIEnv* env, jclass cls) {
    struct sym { unsigned name, value, size; unsigned char info, other; unsigned short shndx; };
    char* si = dlopen("libguestdemo.so", 0);
    if (!si) return 1;
    unsigned base = *(unsigned*)(si + 0x8c);
    const char* strtab = *(const char**)(si + 0xac);
    struct sym* symtab = *(struct sym**)(si + 0xb0);
    unsigned nbucket = *(unsigned*)(si + 0xb4);
    unsigned* bucket = *(unsigned**)(si + 0xbc);
    unsigned* chain = *(unsigned**)(si + 0xc0);
    const char* name = "Java_com_mesflit_arm64bridge_GuestDemo_add";
    for (unsigned n = bucket[old_elf_hash(name) % nbucket]; n; n = chain[n]) {
        const char* a = strtab + symtab[n].name;
        const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a || *b) continue;
        void* found = (void*)(symtab[n].value + base);
        return found == dlsym(si, name) && found == (void*)Java_com_mesflit_arm64bridge_GuestDemo_add ? 0 : 4;
    }
    return 2;
}

/* Old bionic: __page_size, __atomic_inc and SHA1 ("abc" -> a9993e36...). Returns 0 when all work. */
extern unsigned __page_size;
int __atomic_inc(volatile int* p);
typedef struct { unsigned state[5]; unsigned count[2]; unsigned char buffer[64]; } SHA1_CTX;
void SHA1Init(SHA1_CTX* c);
void SHA1Update(SHA1_CTX* c, const unsigned char* d, unsigned n);
void SHA1Final(unsigned char digest[20], SHA1_CTX* c);
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_oldBionic(JNIEnv* env, jclass cls) {
    int failed = 0;
    if (__page_size != 4096) failed |= 1;
    volatile int n = 41;
    if (__atomic_inc(&n) != 41 || n != 42) failed |= 2;
    SHA1_CTX c;
    unsigned char d[20];
    SHA1Init(&c);
    SHA1Update(&c, (const unsigned char*)"abc", 3);
    SHA1Final(d, &c);
    static const unsigned char want[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                           0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    for (int i = 0; i < 20; i++) if (d[i] != want[i]) failed |= 4;
    return failed;
}

/* The way Boehm GC (Mono) stops threads: SIGPWR to each, whose handler acknowledges with a semaphore and
   waits in sigsuspend until SIGXCPU resumes it. One thread runs guest code, one waits in a condition
   variable. Returns 0 when both stop and resume. */
#ifdef __ANDROID__  /* built with the NDK's headers (APK) */
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
typedef sigset_t g_sigset_t;
typedef struct sigaction g_sigaction_t;
typedef sem_t g_sem_t;
typedef pthread_cond_t g_cond_t;
#define G_HANDLER(sa) (sa).sa_handler
#else              /* the minimal headers of the desktop test: 32-bit bionic declarations */
typedef unsigned long g_sigset_t;
typedef struct { void (*handler)(int); g_sigset_t mask; int flags; void (*restorer)(void); } g_sigaction_t;
typedef struct { volatile unsigned int count; int reserved[3]; } g_sem_t;
typedef struct { int value; } g_cond_t;
int sigaction(int sig, const g_sigaction_t* act, g_sigaction_t* old);
int sigemptyset(g_sigset_t* set);
int sigsuspend(const g_sigset_t* mask);
int pthread_kill(pthread_t t, int sig);
int sem_init(g_sem_t* s, int pshared, unsigned value);
int sem_post(g_sem_t* s);
int sem_trywait(g_sem_t* s);
int pthread_cond_wait(g_cond_t* c, pthread_mutex_t* m);
int pthread_cond_broadcast(g_cond_t* c);
#define G_HANDLER(sa) (sa).handler
#endif
#define G_SIGXCPU 24
#define G_SIGPWR 30
static g_sem_t g_ack;
static volatile int g_resumed, g_stop_workers, g_spins;
static pthread_mutex_t g_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static g_cond_t g_wait_cond = {0};
static void on_suspend(int sig) {
    sem_post(&g_ack);
    g_sigset_t m;
    sigemptyset(&m);
    sigsuspend(&m);  /* returns after the restart signal's handler ran */
}
static void on_restart(int sig) { __sync_fetch_and_add(&g_resumed, 1); }
static void* spinner(void* arg) {
    while (!g_stop_workers) g_spins++;
    return 0;
}
static void* waiter(void* arg) {
    pthread_mutex_lock(&g_wait_lock);
    while (!g_stop_workers) pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    pthread_mutex_unlock(&g_wait_lock);
    return 0;
}
static int wait_acks(int n) {
    for (int tries = 0; tries < 300 && n > 0; tries++) {
        while (n > 0 && sem_trywait(&g_ack) == 0) n--;
        if (n > 0) usleep(10000);
    }
    return n;
}
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_stopTheWorld(JNIEnv* env, jclass cls) {
    g_sigaction_t sa;
    memset(&sa, 0, sizeof(sa));
    G_HANDLER(sa) = on_suspend;
    sigaction(G_SIGPWR, &sa, 0);
    G_HANDLER(sa) = on_restart;
    sigaction(G_SIGXCPU, &sa, 0);
    sem_init(&g_ack, 0, 0);
    g_resumed = g_stop_workers = g_spins = 0;
    pthread_t a, b;
    pthread_create(&a, 0, spinner, 0);
    pthread_create(&b, 0, waiter, 0);
    usleep(50000);
    int failed = 0;
    pthread_kill(a, G_SIGPWR);
    pthread_kill(b, G_SIGPWR);
    if (wait_acks(2)) failed |= 1;                 /* both acknowledged: the world is stopped */
    int spins = g_spins;
    usleep(30000);
    if (!(failed & 1) && g_spins != spins) failed |= 2;  /* the spinner really stopped */
    pthread_kill(a, G_SIGXCPU);
    pthread_kill(b, G_SIGXCPU);
    for (int i = 0; i < 300 && g_resumed < 2; i++) usleep(10000);
    if (g_resumed != 2) failed |= 4;
    spins = g_spins;
    usleep(30000);
    if (g_spins == spins) failed |= 8;              /* the spinner runs again */
    g_stop_workers = 1;
    pthread_mutex_lock(&g_wait_lock);
    pthread_cond_broadcast(&g_wait_cond);
    pthread_mutex_unlock(&g_wait_lock);
    pthread_join(a, 0);
    pthread_join(b, 0);
    return failed;
}

/* Like a JIT compiler (Mono): writes code, flushes that range, runs it, rewrites it. 0 when every
   call runs the code that was there at the time. */
__attribute__((target("arm"), noinline)) static void flush_range(char* start, char* end, int raw) {
    if (!raw) { __builtin___clear_cache(start, end); return; }
    register long r0 __asm__("r0") = (long)start;  /* cacheflush(start, end, 0) as a system call */
    register long r1 __asm__("r1") = (long)end;
    register long r2 __asm__("r2") = 0;
    register long r7 __asm__("r7") = 0xf0002;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_jitCode(JNIEnv* env, jclass cls) {
    unsigned* code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | 4 /* PROT_EXEC */, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    for (int i = 0; i < 200; i++) {
        unsigned* fn = code + (i % 4) * 8;   /* reuse four slots: old code at the same address */
        fn[0] = 0xe3a00000u | (i & 0xff);     /* mov r0, #i */
        fn[1] = 0xe12fff1eu;                  /* bx lr */
        flush_range((char*)fn, (char*)(fn + 2), i & 1);
        int got = ((int (*)(void))fn)();
        if (got != (i & 0xff)) return 2;
    }
    munmap(code, 4096);
    return 0;
}

/* Method IDs from FromReflectedMethod, as Unity's AndroidJavaObject gets them: a static method and a constructor. */
JNIEXPORT jstring JNICALL Java_com_mesflit_arm64bridge_GuestDemo_reflected(JNIEnv* env, jclass cls, jobject method, jobject ctor) {
    jmethodID describe = (*env)->FromReflectedMethod(env, method);
    jmethodID init = (*env)->FromReflectedMethod(env, ctor);
    if (!describe || !init) return NULL;
    jvalue args[4];
    args[0].j = 7;
    args[1].d = 0.5;
    args[2].l = (*env)->NewStringUTF(env, "x");
    args[3].f = 2.0f;
    jstring s = (jstring)(*env)->CallStaticObjectMethodA(env, cls, describe, args);
    jclass sbc = (*env)->FindClass(env, "java/lang/StringBuilder");
    jvalue a1[1];
    a1[0].l = s;
    jobject sb = (*env)->NewObjectA(env, sbc, init, a1);
    jmethodID append = (*env)->GetMethodID(env, sbc, "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
    jmethodID to_string = (*env)->GetMethodID(env, sbc, "toString", "()Ljava/lang/String;");
    (*env)->CallObjectMethod(env, sb, append, (*env)->NewStringUTF(env, " sb"));
    return (jstring)(*env)->CallObjectMethod(env, sb, to_string);
}

#ifdef __ANDROID__
#include <fcntl.h>
#else
char* fgets(char* s, int n, FILE* f);
int strncmp(const char* a, const char* b, size_t n);
char* strstr(const char* h, const char* n);
int open(const char* path, int flags, ...);
long read(int fd, void* b, size_t n);
#define O_RDONLY 0
#endif

/* /proc/cpuinfo as a 32-bit ARM process sees it: Mono looks for "thumb" on the Features line. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_cpuinfo(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f) return 1;
    char line[512];
    int processor = 0, thumb = 0, neon = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Processor", 9) == 0 && strstr(line, "(v7")) processor = 1;
        if (strncmp(line, "Features", 8) == 0) {
            if (strstr(line, "thumb")) thumb = 1;
            if (strstr(line, "neon")) neon = 1;
        }
    }
    fclose(f);
    if (!processor) return 2;
    if (!thumb || !neon) return 3;
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) return 4;
    char b[16];
    long n = (long)read(fd, b, sizeof(b));
    close(fd);
    return n == 16 && memcmp(b, "Processor", 9) == 0 ? 0 : 5;
}

/* Ignores an exception thrown by a Java method and goes on with other JNI calls, as old games do. */
JNIEXPORT jint JNICALL Java_com_mesflit_arm64bridge_GuestDemo_ignoredException(JNIEnv* env, jclass cls) {
    jmethodID boom = (*env)->GetStaticMethodID(env, cls, "boom", "()V");
    if (!boom) return 1;
    (*env)->CallStaticVoidMethod(env, cls, boom);
    jclass str = (*env)->FindClass(env, "java/lang/String");
    if (!str) return 2;
    return (*env)->ExceptionCheck(env) ? 3 : 0;
}
