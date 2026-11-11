// SPDX-License-Identifier: GPL-2.0-or-later
#include "common.h"

#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace a64b {

int g_log_level = LOG_INFO;
static std::mutex g_cap_mu;
static std::string* g_capture = nullptr;
static std::atomic<int> g_file_fd{-1};

void log_set_file(int fd) { g_file_fd.store(fd); }
int log_file_fd() { return g_file_fd.load(); }

// One line in the log file: "MM-DD hh:mm:ss.mmm tid L tag: message". Uses only write(),
// so a line is never lost to buffering when the process dies right after.
static void file_line(int level, const char* tag, const char* msg) {
    int fd = g_file_fd.load();
    if (fd < 0) return;
    static const char lv[] = "??VDIWEF";
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    char head[96];
    int n = snprintf(head, sizeof(head), "%02d-%02d %02d:%02d:%02d.%03d %5d %c %s: ", tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), (int)gettid(), lv[level & 7], tag);
    std::string line(head, n > 0 ? (size_t)n : 0);
    line += msg;
    line.push_back('\n');
    ssize_t r = write(fd, line.data(), line.size());
    (void)r;
}

void log_tagged(int level, const char* tag, const char* msg) {
#ifdef __ANDROID__
    __android_log_write(level, tag, msg);
#else
    fprintf(stderr, "[%s %d] %s\n", tag, level, msg);
#endif
    file_line(level, tag, msg);
}

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
    file_line(level, "arm64bridge", buf);
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
