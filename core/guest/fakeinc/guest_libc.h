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
