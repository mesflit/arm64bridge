// Layouts of 32-bit bionic structures and conversion helpers.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <sys/stat.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <sys/time.h>
#include <time.h>

#include "common.h"
#include "guest_mem.h"

namespace a64b {

struct GTimespec { i32 tv_sec; i32 tv_nsec; };
struct GTimeval { i32 tv_sec; i32 tv_usec; };

struct GTm {
    i32 tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    i32 tm_gmtoff;
    u32 tm_zone;
};

struct GStat {
    u64 st_dev;
    u8 pad0[4];
    u32 st_ino32;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u8 pad3[4];
    i64 st_size __attribute__((aligned(8)));
    u32 st_blksize;
    u64 st_blocks __attribute__((aligned(8)));
    GTimespec st_atim, st_mtim, st_ctim;
    u64 st_ino;
};
static_assert(sizeof(GStat) == 104, "guest struct stat layout");

static inline void to_guest(GTimespec* g, const timespec& h) { g->tv_sec = (i32)h.tv_sec; g->tv_nsec = (i32)h.tv_nsec; }
static inline timespec from_guest(const GTimespec* g) { timespec t; t.tv_sec = g->tv_sec; t.tv_nsec = g->tv_nsec; return t; }

static inline void to_guest(GStat* g, const struct stat& s) {
    memset(g, 0, sizeof(*g));
    g->st_dev = s.st_dev;
    g->st_ino32 = (u32)s.st_ino;
    g->st_mode = s.st_mode;
    g->st_nlink = (u32)s.st_nlink;
    g->st_uid = s.st_uid;
    g->st_gid = s.st_gid;
    g->st_rdev = s.st_rdev;
    g->st_size = s.st_size;
    g->st_blksize = (u32)s.st_blksize;
    g->st_blocks = s.st_blocks;
    to_guest(&g->st_atim, s.st_atim);
    to_guest(&g->st_mtim, s.st_mtim);
    to_guest(&g->st_ctim, s.st_ctim);
    g->st_ino = s.st_ino;
}

static inline void to_guest(GTm* g, const struct tm& t, u32 zone) {
    g->tm_sec = t.tm_sec; g->tm_min = t.tm_min; g->tm_hour = t.tm_hour; g->tm_mday = t.tm_mday;
    g->tm_mon = t.tm_mon; g->tm_year = t.tm_year; g->tm_wday = t.tm_wday; g->tm_yday = t.tm_yday;
    g->tm_isdst = t.tm_isdst; g->tm_gmtoff = (i32)t.tm_gmtoff; g->tm_zone = zone;
}
static inline struct tm from_guest(const GTm* g) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec = g->tm_sec; t.tm_min = g->tm_min; t.tm_hour = g->tm_hour; t.tm_mday = g->tm_mday;
    t.tm_mon = g->tm_mon; t.tm_year = g->tm_year; t.tm_wday = g->tm_wday; t.tm_yday = g->tm_yday;
    t.tm_isdst = g->tm_isdst; t.tm_gmtoff = g->tm_gmtoff;
    return t;
}

// Lock-free growable table of host objects addressed by small integers
// (used for objects whose guest representation is only 4 bytes wide).
template <class T>
class HandleTable {
  public:
    static constexpr u32 kChunkBits = 12, kChunk = 1u << kChunkBits, kTop = 1u << 16;
    u32 put(T* obj) {
        u32 idx;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!free_.empty()) {
                idx = free_.back();
                free_.pop_back();
            } else {
                idx = next_++;
                if ((idx >> kChunkBits) >= kTop) fatal("handle table full");
                if (!chunks_[idx >> kChunkBits].load()) chunks_[idx >> kChunkBits].store(new std::atomic<T*>[kChunk]());
            }
        }
        chunks_[idx >> kChunkBits].load()[idx & (kChunk - 1)].store(obj);
        return idx;
    }
    T* get(u32 idx) const {
        if ((idx >> kChunkBits) >= kTop) return nullptr;
        auto* ch = chunks_[idx >> kChunkBits].load(std::memory_order_acquire);
        return ch ? ch[idx & (kChunk - 1)].load(std::memory_order_acquire) : nullptr;
    }
    T* take(u32 idx) {
        T* o = get(idx);
        if (!o) return nullptr;
        chunks_[idx >> kChunkBits].load()[idx & (kChunk - 1)].store(nullptr);
        std::lock_guard<std::mutex> lk(mu_);
        free_.push_back(idx);
        return o;
    }

  private:
    std::mutex mu_;
    u32 next_ = 1;  // index 0 is never used
    std::vector<u32> free_;
    std::atomic<std::atomic<T*>*> chunks_[kTop] = {};
};

}  // namespace a64b
