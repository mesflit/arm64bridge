// SPDX-License-Identifier: GPL-2.0-or-later
#include "common.h"

#include <mutex>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace a64b {

int g_log_level = LOG_INFO;
static std::mutex g_cap_mu;
static std::string* g_capture = nullptr;

void log_set_capture(std::string* sink) {
    std::lock_guard<std::mutex> lk(g_cap_mu);
    g_capture = sink;
}

void log_writev(int level, const char* fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
#ifdef __ANDROID__
    __android_log_write(level, "arm64bridge", buf);
#else
    static const char* const names[] = {"?", "?", "V", "D", "I", "W", "E", "F"};
    fprintf(stderr, "[a64b %s] %s\n", names[level & 7], buf);
#endif
    std::lock_guard<std::mutex> lk(g_cap_mu);
    if (g_capture) {
        g_capture->append(buf);
        g_capture->push_back('\n');
    }
}

void log_write(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_writev(level, fmt, ap);
    va_end(ap);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_writev(LOG_ERROR, fmt, ap);
    va_end(ap);
    abort();
}

std::string strfmt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) vsnprintf(s.data(), n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

}  // namespace a64b
