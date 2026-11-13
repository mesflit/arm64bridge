//! FPS booster for games whose frame limiter waits on the clock.
//!
//! Shortening sleeps does not lift such a limit (the game spins on the clock instead), and running the
//! clock faster would make the whole game faster. What works is changing the game's target frame time.
//! Known limiter functions are found by their code when a library is loaded; when the game calls the
//! clock from that function, the target in the limiter object is replaced according to the booster mode.
//!
//! Known limiters:
//!  - Gameloft engine (Minion Rush and other Gameloft games of that time), ARM code:
//!      void Limiter::Wait(Limiter* this)   this+0: enabled (u8), this+8: frame time in us (s64),
//!                                          this+0x10: start of the last frame in us (s64)
//!    It reads the clock with gettimeofday as its first call; at that point r4 == this.
//!  - Gameloft engine (Minion Rush), second limiter in the game loop, ARM code:
//!      now = GetTimeMs(); wait = last + FRAME_MS - now; if (1 <= wait <= 29) usleep(wait * 1000);
//!    FRAME_MS (30) is the immediate of "rsb r1, r2, #30". The usleep of that site is adjusted.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::sync::atomic::{AtomicI32, AtomicU32, AtomicUsize, Ordering};
use std::sync::Mutex;

use crate::cpu::{Cpu, G_FRAME_LIMITER};
use crate::elf::{describe_address, Module};
use crate::mem::*;
use crate::util::lock;

#[derive(PartialEq, Eq)]
enum Kind {
    TargetInObject,
    SleepMs,
}

struct Pattern {
    kind: Kind,
    name: &'static str,
    words: [u32; 15],
    masks: [u32; 15],
    /// offset of the return address of the clock call (TargetInObject) or of usleep (SleepMs)
    call_offset: u32,
    /// TargetInObject: s64 frame time in us inside the limiter object;
    /// SleepMs: index of the word whose imm8 is the frame time in ms
    target_offset: u32,
}

const X: u32 = !0;
static PATTERNS: [Pattern; 2] = [
    Pattern {
        kind: Kind::TargetInObject,
        name: "Gameloft engine frame limiter",
        words: [
            0xe92d4ff0, 0xe24dd014, 0xe1a04000, 0xe28d000c, 0xe3a01000, 0xeb000000, 0xe59d2010, 0xe3047240, 0xe59d000c, 0xe340700f, 0xe58d4008, 0xe1a03fc2,
            0xe5b46010, 0xe0e32790, 0xe14400d8,
        ],
        masks: [X, X, X, X, X, 0xff000000, X, X, X, X, X, X, X, X, X],
        call_offset: 0x18,
        target_offset: 8,
    },
    Pattern {
        kind: Kind::SleepMs,
        name: "Gameloft game loop frame sleep",
        words: [
            0xe1a02000, 0xe1a03001, 0xe1c500d0, 0xe08f5004, 0xe1901001, 0xe1c520f0, 0x0a000011, 0xe2621000, 0xe0810000, 0xe2401001, 0xe351001c, 0x8a000002,
            0xe3a01ffa, 0xe0000190, 0xeb000000,
        ],
        masks: [X, X, X, X, X, X, X, 0xffffff00, X, X, X, X, X, X, 0xff000000],
        call_offset: 0x3c,
        target_offset: 7,
    },
];

static G_SITE: AtomicU32 = AtomicU32::new(0); // return address of the limiter's clock call (0 = no known limiter)
static G_PATTERN: AtomicUsize = AtomicUsize::new(0); // index into PATTERNS of the limiter found
static G_SLEEP_SITE: AtomicU32 = AtomicU32::new(0); // return address of the frame sleep's usleep call
static G_SLEEP_FRAME_MS: AtomicI32 = AtomicI32::new(0);

// Games can have several limiter objects (for example one per game state).
#[derive(Clone, Copy)]
struct Slot {
    obj: u32,
    original: i64, // the game's own frame time (us)
    written: i64,  // value this file last left in the object
}
const EMPTY: Slot = Slot { obj: 0, original: -1, written: -1 };
static SLOTS: Mutex<([Slot; 8], usize)> = Mutex::new(([EMPTY; 8], 0));

unsafe fn matches(a: u32, pat: &Pattern) -> bool {
    (0..15).all(|i| (rd::<u32>(a + 4 * i as u32) & pat.masks[i]) == pat.words[i])
}

pub fn frame_cap_known() -> bool {
    G_SITE.load(Ordering::Relaxed) != 0 || G_SLEEP_SITE.load(Ordering::Relaxed) != 0
}

pub fn frame_cap_scan(m: &Module) {
    for (index, pat) in PATTERNS.iter().enumerate() {
        let site = if pat.kind == Kind::TargetInObject { &G_SITE } else { &G_SLEEP_SITE };
        if site.load(Ordering::SeqCst) != 0 {
            continue;
        }
        const LEN: u64 = 4 * 15;
        let end = m.base as u64 + m.size as u64;
        let mut cur_page = 1u32;
        let mut mapped = false;
        let mut a = ((m.base + 3) & !3) as u64;
        while a + LEN <= end {
            let at = a as u32;
            // Modules can have unmapped gaps between their segments.
            let page = at & !4095;
            if page != cur_page {
                cur_page = page;
                mapped = mem_is_mapped(page, 4096);
            }
            if !mapped {
                a = page as u64 + 4096;
                continue;
            }
            a += 4;
            if ((at + LEN as u32 - 1) & !4095) != page && !mem_is_mapped(at, LEN as u32) {
                continue;
            }
            if !unsafe { matches(at, pat) } {
                continue;
            }
            if pat.kind == Kind::TargetInObject {
                G_PATTERN.store(index, Ordering::SeqCst);
                G_SITE.store(at + pat.call_offset, Ordering::SeqCst);
                logi!("FPS booster: found the {} at {}", pat.name, describe_address(at));
            } else {
                let ms = (unsafe { rd::<u32>(at + 4 * pat.target_offset) } & 0xff) as i32;
                G_SLEEP_FRAME_MS.store(ms, Ordering::SeqCst);
                G_SLEEP_SITE.store(at + pat.call_offset, Ordering::SeqCst);
                logi!("FPS booster: found the {} ({} ms per frame) at {}", pat.name, ms, describe_address(at));
            }
            break;
        }
    }
}

/// Called from the clock functions on every call; cheap unless the caller is the known limiter.
pub fn frame_cap_on_clock(c: &mut Cpu) {
    let site = G_SITE.load(Ordering::Relaxed);
    if site == 0 || c.r(14) != site {
        return;
    }
    let pat = &PATTERNS[G_PATTERN.load(Ordering::Relaxed)];
    let obj = c.r(4);
    if !mem_is_mapped(obj, pat.target_offset + 8) {
        return;
    }
    let target = obj + pat.target_offset;
    let mut g = lock(&SLOTS);
    let idx = match g.0.iter().position(|s| s.obj == obj) {
        Some(i) => i,
        None => {
            let i = g.1 % 8;
            g.1 += 1;
            g.0[i] = Slot { obj, ..EMPTY };
            i
        }
    };
    let slot = &mut g.0[idx];
    let cur: i64 = unsafe { rd(target) };
    if cur != slot.written && cur != slot.original {
        // First call, or the game set a new frame time itself: that is its own limit from now on.
        slot.original = cur;
        logi!("FPS booster: frame limiter 0x{:08x}: game limit {} us per frame ({:.0} FPS)", obj, cur, if cur > 0 { 1e6 / cur as f64 } else { 0.0 });
    }
    let original = slot.original;
    let want = match G_FRAME_LIMITER.load(Ordering::Relaxed) {
        1 => original.min(16666), // 60 FPS
        2 => 0,                   // no limit (the display still paces frames)
        _ => original,
    };
    if cur != want {
        unsafe { wr::<i64>(target, want) };
    }
    slot.written = want;
}

/// Called from usleep: shortens the known frame sleep according to the booster mode.
pub fn frame_cap_on_sleep(c: &mut Cpu, us: &mut u32) {
    let site = G_SLEEP_SITE.load(Ordering::Relaxed);
    if site == 0 || c.r(14) != site {
        return;
    }
    let frame_ms = G_SLEEP_FRAME_MS.load(Ordering::Relaxed);
    match G_FRAME_LIMITER.load(Ordering::Relaxed) {
        1 => {
            // 60 FPS: the game waits until 16 ms instead of frame_ms have passed
            let cut = if frame_ms > 16 { (frame_ms - 16) as u32 * 1000 } else { 0 };
            *us = us.saturating_sub(cut);
        }
        2 => *us = 0,
        _ => {}
    }
}
