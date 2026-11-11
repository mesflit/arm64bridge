// SPDX-License-Identifier: GPL-2.0-or-later
//
// FPS booster for games whose frame limiter waits on the clock.
//
// Shortening sleeps does not lift such a limit (the game spins on the clock instead), and running the
// clock faster would make the whole game faster. What works is changing the game's target frame time.
// Known limiter functions are found by their code when a library is loaded; when the game calls the
// clock from that function, the target in the limiter object is replaced according to the booster mode.
//
// Known limiters:
//  - Gameloft engine (Minion Rush and other Gameloft games of that time), ARM code:
//      void Limiter::Wait(Limiter* this)   this+0: enabled (u8), this+8: frame time in us (s64),
//                                          this+0x10: start of the last frame in us (s64)
//    It reads the clock with gettimeofday as its first call; at that point r4 == this.

#include "common.h"
#include "cpu.h"
#include "elf_loader.h"
#include "guest_mem.h"

#include <atomic>
#include <cstring>

namespace a64b {
namespace {

struct Pattern {
    const char* name;
    u32 words[15];
    u32 masks[15];
    int count;
    u32 clock_call_offset;  // offset of the return address of the clock call (start of the function = 0)
    u32 target_offset;      // s64 frame time in us inside the limiter object
};

const Pattern PATTERNS[] = {
    {"Gameloft engine frame limiter",
     {0xe92d4ff0, 0xe24dd014, 0xe1a04000, 0xe28d000c, 0xe3a01000, 0xeb000000, 0xe59d2010, 0xe3047240,
      0xe59d000c, 0xe340700f, 0xe58d4008, 0xe1a03fc2, 0xe5b46010, 0xe0e32790, 0xe14400d8},
     {~0u, ~0u, ~0u, ~0u, ~0u, 0xff000000, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u},
     15, 0x18, 8},
};

std::atomic<u32> g_site{0};          // return address of the limiter's clock call (0 = no known limiter)
const Pattern* g_pattern = nullptr;
std::atomic<long long> g_original{-1};  // the game's own frame time (us)
std::atomic<long long> g_written{-1};   // value this file last left in the limiter object

bool matches(const u8* p, const Pattern& pat) {
    for (int i = 0; i < pat.count; i++) {
        u32 w;
        memcpy(&w, p + 4 * i, 4);
        if ((w & pat.masks[i]) != pat.words[i]) return false;
    }
    return true;
}

}  // namespace

bool frame_cap_known() { return g_site.load(std::memory_order_relaxed) != 0; }

void frame_cap_scan(Module* m) {
    if (!m || g_site.load()) return;
    for (const Pattern& pat : PATTERNS) {
        const u32 len = 4 * pat.count;
        u32 cur_page = 1;
        bool mapped = false;
        for (u32 a = (m->base + 3) & ~3u; a + len <= m->base + m->size; a += 4) {
            // Modules can have unmapped gaps between their segments.
            u32 page = a & ~4095u;
            if (page != cur_page) {
                cur_page = page;
                mapped = mem_is_mapped(page, 4096);
            }
            if (!mapped) {
                a = page + 4096 - 4;
                continue;
            }
            if (((a + len - 1) & ~4095u) != page && !mem_is_mapped(a, len)) continue;
            if (!matches(g2h<u8>(a), pat)) continue;
            g_pattern = &pat;
            g_site.store(a + pat.clock_call_offset);
            LOGI("FPS booster: found the %s at %s", pat.name, describe_address(a).c_str());
            return;
        }
    }
}

// Called from the clock functions on every call; cheap unless the caller is the known limiter.
void frame_cap_on_clock(Cpu& c) {
    u32 site = g_site.load(std::memory_order_relaxed);
    if (!site || c.r[14] != site) return;
    u32 obj = c.r[4];
    if (!mem_is_mapped(obj, g_pattern->target_offset + 8)) return;
    long long* target = g2h<long long>(obj + g_pattern->target_offset);
    long long cur = *target;
    if (cur != g_written.load(std::memory_order_relaxed)) {
        // First call, or the game set a new frame time itself: that is its own limit from now on.
        if (g_original.exchange(cur) != cur)
            LOGI("FPS booster: game frame limit %lld us per frame (%.0f FPS)", cur, cur > 0 ? 1e6 / (double)cur : 0.0);
    }
    long long original = g_original.load(std::memory_order_relaxed), want = original;
    switch (g_frame_limiter.load(std::memory_order_relaxed)) {
        case 1: want = original > 16666 ? 16666 : original; break;  // 60 FPS
        case 2: want = 0; break;                                    // no limit (the display still paces frames)
        default: break;
    }
    if (cur != want) *target = want;
    g_written.store(want, std::memory_order_relaxed);
}

}  // namespace a64b
