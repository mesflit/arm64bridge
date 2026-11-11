// ARM64Bridge - common definitions
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace a64b {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

enum LogLevel { LOG_DEBUG = 3, LOG_INFO = 4, LOG_WARN = 5, LOG_ERROR = 6 };

void log_write(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void log_writev(int level, const char* fmt, va_list ap);
// Optional sink that receives a copy of every log line (used by the self test UI).
void log_set_capture(std::string* sink);
extern int g_log_level;
// Mirrors every log line into an open file descriptor (-1 disables).
void log_set_file(int fd);
int log_file_fd();
// Log a line under another tag (used for messages written by guest code).
void log_tagged(int level, const char* tag, const char* msg);
// Installs handlers that write a crash report to the log before the process dies.
void crash_handler_install();

#define LOGD(...) do { if (::a64b::g_log_level <= ::a64b::LOG_DEBUG) ::a64b::log_write(::a64b::LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGI(...) ::a64b::log_write(::a64b::LOG_INFO, __VA_ARGS__)
#define LOGW(...) ::a64b::log_write(::a64b::LOG_WARN, __VA_ARGS__)
#define LOGE(...) ::a64b::log_write(::a64b::LOG_ERROR, __VA_ARGS__)

[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string strfmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace a64b
