/* 32-bit bionic declarations used by guestdemo.c when no NDK sysroot is available. */
#pragma once
#include <stddef.h>
#include <stdint.h>
void* malloc(size_t);
void free(void*);
void* memcpy(void*, const void*, size_t);
void* memset(void*, int, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);
int memcmp(const void*, const void*, size_t);
char* strcpy(char*, const char*);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
double sqrt(double);
float sinf(float);
double pow(double, double);
typedef long pthread_t;
typedef struct { int32_t value; } pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {0}
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_unlock(pthread_mutex_t*);
int __android_log_print(int, const char*, const char*, ...);
typedef int jmp_buf[64];
int setjmp(jmp_buf);
void longjmp(jmp_buf, int) __attribute__((noreturn));

/* zlib (32-bit layout) */
typedef struct z_stream_s {
    const unsigned char* next_in; unsigned avail_in; unsigned long total_in;
    unsigned char* next_out; unsigned avail_out; unsigned long total_out;
    const char* msg; void* state; void* zalloc; void* zfree; void* opaque;
    int data_type; unsigned long adler; unsigned long reserved;
} z_stream;
#define ZLIB_VERSION "1.2.13"
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_FINISH 4
#define Z_NO_FLUSH 0
const char* zlibVersion(void);
unsigned long crc32(unsigned long, const unsigned char*, unsigned);
unsigned long compressBound(unsigned long);
int compress(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
int uncompress(unsigned char*, unsigned long*, const unsigned char*, unsigned long);
int deflateInit_(z_stream*, int, const char*, int);
int inflateInit2_(z_stream*, int, const char*, int);
int deflate(z_stream*, int);
int inflate(z_stream*, int);
int deflateEnd(z_stream*);
int inflateEnd(z_stream*);
#define deflateInit(s, l) deflateInit_((s), (l), ZLIB_VERSION, (int)sizeof(z_stream))
#define inflateInit2(s, w) inflateInit2_((s), (w), ZLIB_VERSION, (int)sizeof(z_stream))
/* epoll / eventfd */
struct epoll_event { uint32_t events; uint64_t data; };
#define EPOLLIN 1
#define EPOLL_CTL_ADD 1
int epoll_create(int);
int epoll_ctl(int, int, int, struct epoll_event*);
int epoll_wait(int, struct epoll_event*, int, int);
int eventfd(unsigned, int);
long write(int, const void*, size_t);
int close(int);
typedef struct { int quot; int rem; } div_t;
div_t div(int, int);
typedef unsigned wchar_t32;
int swprintf(wchar_t32*, size_t, const wchar_t32*, ...);
typedef long wctype_t;
wctype_t wctype(const char*);
int iswctype(unsigned, wctype_t);
