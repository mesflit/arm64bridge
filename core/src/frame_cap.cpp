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
//  - Gameloft engine (Minion Rush), second limiter in the game loop, ARM code:
//      now = GetTimeMs(); wait = last + FRAME_MS - now; if (1 <= wait <= 29) usleep(wait * 1000);
//    FRAME_MS (30) is the immediate of "rsb r1, r2, #30". The usleep of that site is adjusted.

#include "common.h"
#include "cpu.h"
#include "elf_loader.h"
#include "guest_mem.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace a64b {
namespace {

enum class Kind { TARGET_IN_OBJECT, SLEEP_MS };

struct Pattern {
    Kind kind;
    const char* name;
    u32 words[15];
    u32 masks[15];
    int count;
    u32 call_offset;    // offset of the return address of the clock call (TARGET_IN_OBJECT) or of usleep (SLEEP_MS)
    u32 target_offset;  // TARGET_IN_OBJECT: s64 frame time in us inside the limiter object;
                        // SLEEP_MS: index of the word whose imm8 is the frame time in ms
};

const Pattern PATTERNS[] = {
    {Kind::TARGET_IN_OBJECT, "Gameloft engine frame limiter",
     {0xe92d4ff0, 0xe24dd014, 0xe1a04000, 0xe28d000c, 0xe3a01000, 0xeb000000, 0xe59d2010, 0xe3047240,
      0xe59d000c, 0xe340700f, 0xe58d4008, 0xe1a03fc2, 0xe5b46010, 0xe0e32790, 0xe14400d8},
     {~0u, ~0u, ~0u, ~0u, ~0u, 0xff000000, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u},
     15, 0x18, 8},
    {Kind::SLEEP_MS, "Gameloft game loop frame sleep",
     {0xe1a02000, 0xe1a03001, 0xe1c500d0, 0xe08f5004, 0xe1901001, 0xe1c520f0, 0x0a000011, 0xe2621000,
      0xe0810000, 0xe2401001, 0xe351001c, 0x8a000002, 0xe3a01ffa, 0xe0000190, 0xeb000000},
     {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0xffffff00, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0xff000000},
     15, 0x3c, 7},
};

std::atomic<u32> g_site{0};          // return address of the limiter's clock call (0 = no known limiter)
const Pattern* g_pattern = nullptr;
std::atomic<u32> g_sleep_site{0};    // return address of the frame sleep's usleep call
std::atomic<int> g_sleep_frame_ms{0};
// Games can have several limiter objects (for example one per game state).
struct Slot {
    u32 obj = 0;
    long long original = -1;  // the game's own frame time (us)
    long long written = -1;   // value this file last left in the object
};
std::mutex g_slots_mu;
Slot g_slots[8];
int g_next_slot = 0;

bool matches(const u8* p, const Pattern& pat) {
    for (int i = 0; i < pat.count; i++) {
        u32 w;
        memcpy(&w, p + 4 * i, 4);
        if ((w & pat.masks[i]) != pat.words[i]) return false;
    }
    return true;
}

}  // namespace

bool frame_cap_known() {
    return g_site.load(std::memory_order_relaxed) != 0 || g_sleep_site.load(std::memory_order_relaxed) != 0;
}

void frame_cap_scan(Module* m) {
    if (!m) return;
    for (const Pattern& pat : PATTERNS) {
        if ((pat.kind == Kind::TARGET_IN_OBJECT ? g_site : g_sleep_site).load()) continue;
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
            if (pat.kind == Kind::TARGET_IN_OBJECT) {
                g_pattern = &pat;
                g_site.store(a + pat.call_offset);
                LOGI("FPS booster: found the %s at %s", pat.name, describe_address(a).c_str());
            } else {
                int ms = (int)(*g2h<u32>(a + 4 * pat.target_offset) & 0xff);
                g_sleep_frame_ms.store(ms);
                g_sleep_site.store(a + pat.call_offset);
                LOGI("FPS booster: found the %s (%d ms per frame) at %s", pat.name, ms, describe_address(a).c_str());
            }
            break;
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
    std::lock_guard<std::mutex> lk(g_slots_mu);
    Slot* slot = nullptr;
    for (Slot& sl : g_slots)
        if (sl.obj == obj) slot = &sl;
    if (!slot) {
        slot = &g_slots[g_next_slot++ % 8];
        *slot = Slot();
        slot->obj = obj;
    }
    long long cur = *target;
    if (cur != slot->written && cur != slot->original) {
        // First call, or the game set a new frame time itself: that is its own limit from now on.
        slot->original = cur;
        LOGI("FPS booster: frame limiter 0x%08x: game limit %lld us per frame (%.0f FPS)", obj, cur, cur > 0 ? 1e6 / (double)cur : 0.0);
    }
    long long original = slot->original, want = original;
    switch (g_frame_limiter.load(std::memory_order_relaxed)) {
        case 1: want = original > 16666 ? 16666 : original; break;  // 60 FPS
        case 2: want = 0; break;                                    // no limit (the display still paces frames)
        default: break;
    }
    if (cur != want) *target = want;
    slot->written = want;
}

// Called from usleep: shortens the known frame sleep according to the booster mode.
void frame_cap_on_sleep(Cpu& c, u32& us) {
    u32 site = g_sleep_site.load(std::memory_order_relaxed);
    if (!site || c.r[14] != site) return;
    int frame_ms = g_sleep_frame_ms.load(std::memory_order_relaxed);
    switch (g_frame_limiter.load(std::memory_order_relaxed)) {
        case 1: {  // 60 FPS: the game waits until 16 ms instead of frame_ms have passed
            u32 cut = frame_ms > 16 ? (u32)(frame_ms - 16) * 1000 : 0;
            us = us > cut ? us - cut : 0;
            break;
        }
        case 2: us = 0; break;
        default: break;
    }
}

}  // namespace a64b
