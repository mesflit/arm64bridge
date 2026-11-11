/* Minimal declarations for building the guest demo without an ARM sysroot (host tests only). */
#pragma once
#include <stdarg.h>
#include <stddef.h>
typedef struct __sFILE FILE;
int snprintf(char*, size_t, const char*, ...);
int sscanf(const char*, const char*, ...);
