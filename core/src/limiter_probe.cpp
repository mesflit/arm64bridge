// SPDX-License-Identifier: GPL-2.0-or-later
//
// Frame limiter probe: finds out where a game's own frame limiter is.
//
// Many games cap their frame rate by waiting until a fixed frame time has passed (sleeping and/or
// spinning on the clock). Shortening the sleeps does not help such games, and making the clock run
// faster would make the whole game run faster. To raise the cap, the game's target frame time has to
// be changed, so the first step is to know which code waits. The probe records, on the thread that
// presents frames, which guest functions call the clock and sleep functions, and writes the most
// frequent call sites (with their code bytes) to the log a few times per session.

#include "common.h"
#include "cpu.h"
#include "elf_loader.h"
#include "guest_mem.h"
#include "thunk.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

namespace a64b {
namespace {

const char* const KIND_NAMES[] = {"gettimeofday", "clock_gettime", "usleep/nanosleep", "sched_yield"};
constexpr int KINDS = 4;
constexpr int SAMPLE_EVERY = 128;
constexpr u64 REPORT_FRAMES[] = {900, 2700, 5400};  // about 30 s, 90 s and 3 min at 30 fps

struct Site {
    u32 lr = 0, caller1 = 0, caller2 = 0;
    bool operator<(const Site& o) const { return std::tie(lr, caller1, caller2) < std::tie(o.lr, o.caller1, o.caller2); }
};

struct Probe {
    int tid = 0;
    u64 calls[KINDS] = {};
    std::map<Site, u64> sites[KINDS];
    u64 sleep_us_sum = 0, sleep_count = 0, sleep_min = ~0ull, sleep_max = 0;
    u64 frames = 0;
    int reports = 0;
    std::vector<std::pair<u32, u32>> text;  // code ranges of the loaded guest libraries
};

thread_local Probe* t_probe = nullptr;

bool is_code(Probe& p, u32 a) {
    for (auto& r : p.text)
        if (a >= r.first && a < r.second) return true;
    return false;
}

Probe* probe_for_this_thread() {
    static thread_local int tid = (int)syscall(SYS_gettid);
    if (tid != g_render_tid.load(std::memory_order_relaxed)) return nullptr;
    if (!t_probe) {
        t_probe = new Probe();
        t_probe->tid = tid;
        for (Module* m : loader_modules()) t_probe->text.emplace_back(m->base, m->base + m->size);
    }
    return t_probe->reports < (int)(sizeof(REPORT_FRAMES) / sizeof(REPORT_FRAMES[0])) ? t_probe : nullptr;
}

void dump_code(u32 addr) {
    addr &= ~1u;
    u32 start = (addr - 256) & ~15u, end = addr + 256;
    for (u32 a = start; a < end; a += 16) {
        if (!mem_is_mapped(a, 16)) continue;
        const u8* b = g2h<u8>(a);
        std::string line = strfmt("    %08x:", a);
        for (int i = 0; i < 16; i++) line += strfmt(i % 4 == 0 ? " %02x" : "%02x", b[i]);
        LOGI("%s", line.c_str());
    }
}

void report(Probe& p) {
    p.reports++;
    LOGI("frame limiter probe %d (render thread %d, %llu frames): calls per frame:", p.reports, p.tid, (unsigned long long)p.frames);
    for (int k = 0; k < KINDS; k++)
        LOGI("  %s: %.1f", KIND_NAMES[k], p.frames ? (double)p.calls[k] / (double)p.frames : 0.0);
    if (p.sleep_count)
        LOGI("  sleeps: %llu, %.0f us on average (min %llu, max %llu)", (unsigned long long)p.sleep_count,
             (double)p.sleep_us_sum / (double)p.sleep_count, (unsigned long long)p.sleep_min, (unsigned long long)p.sleep_max);
    std::vector<u32> dumped;
    for (int k = 0; k < KINDS; k++) {
        std::vector<std::pair<u64, Site>> top;
        for (auto& e : p.sites[k]) top.emplace_back(e.second, e.first);
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < top.size() && i < 3; i++) {
            const Site& s = top[i].second;
            LOGI("  %s called from %s (%llu samples)", KIND_NAMES[k], describe_address(s.lr & ~1u).c_str(), (unsigned long long)top[i].first);
            if (s.caller1) LOGI("      via %s", describe_address(s.caller1 & ~1u).c_str());
            if (s.caller2) LOGI("      via %s", describe_address(s.caller2 & ~1u).c_str());
            // Code of the two busiest sites of each kind, once per report.
            if (i < 2 && std::find(dumped.begin(), dumped.end(), s.lr) == dumped.end()) {
                dumped.push_back(s.lr);
                LOGI("    code around %08x (%s):", s.lr & ~1u, (s.lr & 1) ? "thumb" : "arm");
                dump_code(s.lr);
            }
        }
        p.sites[k].clear();
        p.calls[k] = 0;
    }
    p.sleep_us_sum = p.sleep_count = p.sleep_max = 0;
    p.sleep_min = ~0ull;
    p.frames = 0;
}

}  // namespace

void probe_time_call(int kind, u64 sleep_us) {
    Probe* p = probe_for_this_thread();
    if (!p) return;
    u64 n = p->calls[kind]++;
    if (kind == 2) {
        p->sleep_us_sum += sleep_us;
        p->sleep_count++;
        p->sleep_min = std::min(p->sleep_min, sleep_us);
        p->sleep_max = std::max(p->sleep_max, sleep_us);
    }
    if (n % SAMPLE_EVERY) return;
    Cpu* c = Cpu::current_or_null();
    if (!c) return;
    Site s;
    s.lr = c->r[14];
    // The callers of the calling function: code addresses on the guest stack.
    u32 sp = c->r[13];
    if (sp >= c->stack_base && sp < c->stack_base + c->stack_size) {
        int found = 0;
        for (u32 a = sp; a + 4 <= c->stack_base + c->stack_size && a < sp + 512 && found < 2; a += 4) {
            u32 v = *g2h<u32>(a);
            if (!is_code(*p, v & ~1u) || (v & ~1u) == (s.lr & ~1u)) continue;
            (found == 0 ? s.caller1 : s.caller2) = v;
            found++;
        }
    }
    p->sites[kind][s]++;
}

void probe_frame() {
    Probe* p = probe_for_this_thread();
    if (!p) return;
    p->frames++;
    static u64 total = 0;
    total++;
    if (total == REPORT_FRAMES[p->reports]) report(*p);
}

}  // namespace a64b
