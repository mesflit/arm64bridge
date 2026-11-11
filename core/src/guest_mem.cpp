// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_mem.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace a64b {

static u32 g_base = 0, g_size = 0, g_page = 4096;
static std::mutex g_page_mu;
static std::map<u32, u32> g_free_regions;            // start -> size
struct Region { u32 size; bool exec; };
static std::map<u32, Region> g_used_regions;         // start -> info
static std::atomic<u64> g_epoch{1};

u32 mem_arena_base() { return g_base; }
u32 mem_arena_size() { return g_size; }
u32 mem_page_size() { return g_page; }
bool mem_contains(u64 a) { return a >= g_base && a < (u64)g_base + g_size; }
u64 mem_code_epoch() { return g_epoch.load(std::memory_order_relaxed); }
void mem_bump_code_epoch() { g_epoch.fetch_add(1, std::memory_order_relaxed); }

bool mem_is_mapped(u32 addr, u32 len) {
    if (!mem_contains(addr) || !mem_contains((u64)addr + len - 1)) return false;
    std::unique_lock<std::mutex> lk(g_page_mu, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    auto it = g_used_regions.upper_bound(addr);
    if (it == g_used_regions.begin()) return false;
    --it;
    return (u64)addr + len <= (u64)it->first + it->second.size;
}

static u32 round_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

bool mem_init() {
    if (g_base) return true;
    long ps = sysconf(_SC_PAGESIZE);
    g_page = ps > 4096 ? (u32)ps : 4096;

    // Find the free gaps below 4 GiB.
    std::vector<std::pair<u64, u64>> gaps;
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) {
        LOGE("cannot read /proc/self/maps");
        return false;
    }
    const u64 kLow = 0x10000000ull, kHigh = 0xFFFF0000ull;  // stay away from NULL and the very top
    u64 prev_end = kLow;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long s, e;
        if (sscanf(line, "%llx-%llx", &s, &e) != 2) continue;
        if (s >= kHigh) {
            break;
        }
        if (s > prev_end) gaps.emplace_back(prev_end, s);
        if (e > prev_end) prev_end = e;
    }
    fclose(f);
    if (prev_end < kHigh) gaps.emplace_back(prev_end, kHigh);
    std::sort(gaps.begin(), gaps.end(), [](auto& a, auto& b) { return (a.second - a.first) > (b.second - b.first); });

    u64 want = 1024ull << 20;
    if (const char* e = getenv("A64B_ARENA_MB")) want = strtoull(e, nullptr, 10) << 20;
    const u64 kAlign = 1u << 20;
    for (auto& gap : gaps) {
        u64 lo = (gap.first + kAlign - 1) & ~(kAlign - 1);
        u64 hi = gap.second & ~(kAlign - 1);
        if (hi <= lo + (64ull << 20)) continue;
        // Leave room for the runtime (ART also allocates below 4 GiB): never take more than 3/4 of a gap.
        u64 size = std::min<u64>(want, (hi - lo) / 4 * 3) & ~(kAlign - 1);
        // Take the top of the gap; low addresses are what other low-memory users pick first.
        u64 start = hi - size;
        void* p = mmap((void*)start, size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if ((u64)p != start) {  // kernel without MAP_FIXED_NOREPLACE treated it as a hint
            munmap(p, size);
            continue;
        }
        g_base = (u32)start;
        g_size = (u32)size;
        g_free_regions[g_base] = g_size;
        LOGI("guest arena: 0x%08x - 0x%08x (%u MiB), host page %u", g_base, g_base + g_size, g_size >> 20, g_page);
        return true;
    }
    LOGE("no usable address space below 4 GiB for the guest arena");
    return false;
}

static u32 take_region(u32 size) {
    for (auto it = g_free_regions.begin(); it != g_free_regions.end(); ++it) {
        if (it->second >= size) {
            u32 start = it->first, rest = it->second - size;
            g_free_regions.erase(it);
            if (rest) g_free_regions[start + size] = rest;
            return start;
        }
    }
    return 0;
}

static void give_region(u32 start, u32 size) {
    auto next = g_free_regions.lower_bound(start);
    if (next != g_free_regions.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            size += prev->second;
            g_free_regions.erase(prev);
        }
    }
    if (next != g_free_regions.end() && start + size == next->first) {
        size += next->second;
        g_free_regions.erase(next);
    }
    g_free_regions[start] = size;
}

u32 page_alloc(u32 size, bool exec) {
    if (!size) return 0;
    size = round_up(size, g_page);
    std::lock_guard<std::mutex> lk(g_page_mu);
    u32 a = take_region(size);
    if (!a) {
        LOGE("guest arena exhausted (request %u bytes)", size);
        return 0;
    }
    if (mprotect((void*)(uintptr_t)a, size, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect failed for guest pages");
        give_region(a, size);
        return 0;
    }
    g_used_regions[a] = Region{size, exec};
    return a;
}

u32 page_map_file(u32 size, int host_prot, int host_flags, int fd, i64 offset) {
    if (!size) return 0;
    size = round_up(size, g_page);
    std::lock_guard<std::mutex> lk(g_page_mu);
    u32 a = take_region(size);
    if (!a) return 0;
    int flags = (host_flags & ~(MAP_FIXED_NOREPLACE)) | MAP_FIXED;
    // The host never executes guest code, and the emulator needs read access.
    int prot = (host_prot & PROT_WRITE) | PROT_READ;
    void* p = mmap((void*)(uintptr_t)a, size, prot, flags, fd, offset);
    if (p == MAP_FAILED) {
        give_region(a, size);
        return 0;
    }
    g_used_regions[a] = Region{size, (host_prot & PROT_EXEC) != 0};
    return a;
}

bool page_free(u32 addr, u32 size) {
    size = round_up(size, g_page);
    if (!mem_contains(addr) || !size || (addr & (g_page - 1))) return false;
    std::lock_guard<std::mutex> lk(g_page_mu);
    // Find the region containing addr; partial unmaps split the bookkeeping.
    auto it = g_used_regions.upper_bound(addr);
    if (it == g_used_regions.begin()) return false;
    --it;
    u32 rs = it->first, rsz = it->second.size;
    bool exec = it->second.exec;
    if (addr + size > rs + rsz) size = rs + rsz - addr;
    if (addr >= rs + rsz) return false;
    g_used_regions.erase(it);
    if (addr > rs) g_used_regions[rs] = Region{addr - rs, exec};
    if (addr + size < rs + rsz) g_used_regions[addr + size] = Region{rs + rsz - (addr + size), exec};
    // Drop the pages and return to the reserved state.
    mmap((void*)(uintptr_t)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    give_region(addr, size);
    if (exec) mem_bump_code_epoch();
    return true;
}

void page_mark_exec(u32 addr, u32 size) {
    (void)size;
    std::lock_guard<std::mutex> lk(g_page_mu);
    auto it = g_used_regions.upper_bound(addr);
    if (it == g_used_regions.begin()) return;
    --it;
    if (addr < it->first + it->second.size) it->second.exec = true;
}

u32 h2g(const void* p) {
    uintptr_t v = (uintptr_t)p;
    if (v > 0xFFFFFFFFull) {
        LOGE("host pointer %p is not representable in the 32-bit guest", p);
        return 0;
    }
    return (u32)v;
}

// ---------------------------------------------------------------------------
// malloc: size classes carved from slabs, large blocks straight from pages.
// Every block has an 8 byte header: { u32 tag; u32 value }.
// ---------------------------------------------------------------------------
namespace {

constexpr u32 kTagSmall = 0xA64B5000;   // | class index
constexpr u32 kTagLarge = 0xA64B1A00;   // value = total mapped size
constexpr u32 kTagAligned = 0xA64BA100; // value = original user pointer
constexpr u32 kTagFree = 0xA64BF4EE;
constexpr u32 kClassSizes[] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
                               1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768};
constexpr int kNumClasses = sizeof(kClassSizes) / sizeof(kClassSizes[0]);
constexpr u32 kSlab = 256 * 1024;

struct Hdr { u32 tag; u32 value; };

std::mutex g_malloc_mu;
u32 g_freelist[kNumClasses];

int class_for(u32 size) {
    for (int i = 0; i < kNumClasses; i++)
        if (size <= kClassSizes[i]) return i;
    return -1;
}

Hdr* hdr_of(u32 ptr) { return g2h<Hdr>(ptr - 8); }

}  // namespace

u32 g_malloc(u32 size) {
    if (size == 0) size = 1;
    int c = class_for(size);
    if (c < 0) {
        if (size > 0xF0000000u) return 0;
        u32 total = round_up(size + 8, g_page);
        u32 a = page_alloc(total);
        if (!a) return 0;
        Hdr* h = g2h<Hdr>(a);
        h->tag = kTagLarge;
        h->value = total;
        return a + 8;
    }
    std::lock_guard<std::mutex> lk(g_malloc_mu);
    if (!g_freelist[c]) {
        u32 cell = kClassSizes[c] + 8;
        u32 slab = page_alloc(kSlab);
        if (!slab) return 0;
        u32 n = kSlab / cell;
        for (u32 i = 0; i < n; i++) {
            u32 user = slab + i * cell + 8;
            *g2h<u32>(user) = g_freelist[c];
            g_freelist[c] = user;
        }
    }
    u32 user = g_freelist[c];
    g_freelist[c] = *g2h<u32>(user);
    Hdr* h = hdr_of(user);
    h->tag = kTagSmall | (u32)c;
    h->value = size;
    return user;
}

void g_free(u32 ptr) {
    if (!ptr) return;
    if (!mem_contains(ptr) || (ptr & 7)) {
        LOGW("free(0x%08x): not a guest heap pointer, ignored", ptr);
        return;
    }
    Hdr* h = hdr_of(ptr);
    if (h->tag == kTagAligned) {
        u32 orig = h->value;
        h->tag = kTagFree;
        g_free(orig);
        return;
    }
    if (h->tag == kTagLarge) {
        page_free(ptr - 8, h->value);
        return;
    }
    if ((h->tag & 0xFFFFFF00u) == kTagSmall) {
        int c = (int)(h->tag & 0xFF);
        if (c >= kNumClasses) return;
        std::lock_guard<std::mutex> lk(g_malloc_mu);
        h->tag = kTagFree;
        *g2h<u32>(ptr) = g_freelist[c];
        g_freelist[c] = ptr;
        return;
    }
    LOGW("free(0x%08x): bad or double free, ignored", ptr);
}

u32 g_usable_size(u32 ptr) {
    if (!ptr || !mem_contains(ptr)) return 0;
    Hdr* h = hdr_of(ptr);
    if (h->tag == kTagLarge) return h->value - 8;
    if ((h->tag & 0xFFFFFF00u) == kTagSmall && (h->tag & 0xFF) < (u32)kNumClasses) return kClassSizes[h->tag & 0xFF];
    if (h->tag == kTagAligned) {
        u32 orig = h->value;
        return g_usable_size(orig) - (ptr - orig);
    }
    return 0;
}

u32 g_calloc(u32 n, u32 size) {
    u64 total = (u64)n * size;
    if (total > 0xF0000000u) return 0;
    u32 p = g_malloc((u32)total);
    if (p) memset(g2h(p), 0, total);
    return p;
}

u32 g_realloc(u32 ptr, u32 size) {
    if (!ptr) return g_malloc(size);
    if (size == 0) {
        g_free(ptr);
        return 0;
    }
    u32 old = g_usable_size(ptr);
    if (old >= size && (old <= 64 || size >= old / 2)) return ptr;
    u32 n = g_malloc(size);
    if (!n) return 0;
    memcpy(g2h(n), g2h(ptr), old < size ? old : size);
    g_free(ptr);
    return n;
}

u32 g_memalign(u32 align, u32 size) {
    if (align <= 8) return g_malloc(size);
    if (align & (align - 1)) return 0;
    u32 raw = g_malloc(size + align + 8);
    if (!raw) return 0;
    u32 user = (raw + 8 + align - 1) & ~(align - 1);
    Hdr* h = hdr_of(user);
    h->tag = kTagAligned;
    h->value = raw;
    return user;
}

u32 g_strdup(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    u32 p = g_malloc((u32)n);
    if (p) memcpy(g2h(p), s, n);
    return p;
}

}  // namespace a64b
