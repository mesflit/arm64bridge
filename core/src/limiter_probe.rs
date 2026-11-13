//! Frame limiter probe: finds out where a game's own frame limiter is.
//!
//! Many games cap their frame rate by waiting until a fixed frame time has passed (sleeping and/or
//! spinning on the clock). Shortening the sleeps does not help such games, and making the clock run
//! faster would make the whole game run faster. To raise the cap, the game's target frame time has to
//! be changed, so the first step is to know which code waits. The probe records, on the thread that
//! presents frames, which guest functions call the clock and sleep functions, and writes the most
//! frequent call sites (with their code bytes) to the log a few times per session.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::cpu::{Cpu, G_RENDER_TID};
use crate::elf::{describe_address, loader_modules};
use crate::mem::*;

const KIND_NAMES: [&str; 4] = ["gettimeofday", "clock_gettime", "usleep/nanosleep", "sched_yield"];
const KINDS: usize = 4;
const SAMPLE_EVERY: u64 = 128;
const REPORT_FRAMES: [u64; 3] = [900, 2700, 5400]; // about 30 s, 90 s and 3 min at 30 fps

#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord)]
struct Site {
    lr: u32,
    caller1: u32,
    caller2: u32,
}

struct Probe {
    tid: i32,
    calls: [u64; KINDS],
    sites: [BTreeMap<Site, u64>; KINDS],
    sleep_us_sum: u64,
    sleep_count: u64,
    sleep_min: u64,
    sleep_max: u64,
    frames: u64,
    reports: usize,
    text: Vec<(u32, u32)>, // code ranges of the loaded guest libraries
}

thread_local! {
    static TID: i32 = crate::util::gettid();
    static PROBE: RefCell<Option<Probe>> = const { RefCell::new(None) };
}

fn text_ranges() -> Vec<(u32, u32)> {
    loader_modules().iter().map(|m| (m.base, m.base + m.size)).collect()
}

// Runs f with the probe of this thread if it is the render thread and reports are still due.
fn with_probe(f: impl FnOnce(&mut Probe)) {
    let tid = TID.with(|t| *t);
    if tid != G_RENDER_TID.load(Ordering::Relaxed) {
        return;
    }
    PROBE.with(|p| {
        let Ok(mut p) = p.try_borrow_mut() else { return };
        let probe = p.get_or_insert_with(|| Probe {
            tid,
            calls: [0; KINDS],
            sites: Default::default(),
            sleep_us_sum: 0,
            sleep_count: 0,
            sleep_min: u64::MAX,
            sleep_max: 0,
            frames: 0,
            reports: 0,
            text: text_ranges(),
        });
        if probe.reports < REPORT_FRAMES.len() {
            f(probe);
        }
    });
}

fn report(p: &mut Probe) {
    p.reports += 1;
    logi!("frame limiter probe {} (render thread {}, {} frames): calls per frame:", p.reports, p.tid, p.frames);
    for k in 0..KINDS {
        logi!("  {}: {:.1}", KIND_NAMES[k], if p.frames != 0 { p.calls[k] as f64 / p.frames as f64 } else { 0.0 });
    }
    if p.sleep_count != 0 {
        logi!("  sleeps: {}, {:.0} us on average (min {}, max {})", p.sleep_count, p.sleep_us_sum as f64 / p.sleep_count as f64, p.sleep_min, p.sleep_max);
    }
    let mut dumped: Vec<u32> = Vec::new();
    for k in 0..KINDS {
        let mut top: Vec<(u64, Site)> = p.sites[k].iter().map(|(s, &n)| (n, *s)).collect();
        top.sort_by(|a, b| b.0.cmp(&a.0));
        for (n, s) in top.iter().take(3) {
            logi!("  {} called from {} ({} samples)", KIND_NAMES[k], describe_address(s.lr & !1), n);
            if s.caller1 != 0 {
                logi!("      via {}", describe_address(s.caller1 & !1));
            }
            if s.caller2 != 0 {
                logi!("      via {}", describe_address(s.caller2 & !1));
            }
            // Code of the busiest sites of each kind and of their callers (a clock helper is often
            // called from the loop that actually waits).
            for at in [s.lr, s.caller1] {
                if at == 0 || dumped.contains(&at) {
                    continue;
                }
                dumped.push(at);
                logi!("    code around {:08x} ({}):", at & !1, if at & 1 != 0 { "thumb" } else { "arm" });
                dump_guest_code(at, 384, 256);
            }
        }
        p.sites[k].clear();
        p.calls[k] = 0;
    }
    p.sleep_us_sum = 0;
    p.sleep_count = 0;
    p.sleep_max = 0;
    p.sleep_min = u64::MAX;
    p.frames = 0;
}

/// Hex dump of guest code around addr.
pub fn dump_guest_code(addr: u32, before: u32, after: u32) {
    let addr = addr & !1;
    let start = addr.wrapping_sub(before) & !15;
    let end = addr.wrapping_add(after);
    let mut a = start;
    while a < end {
        if mem_is_mapped(a, 16) {
            let b: [u8; 16] = unsafe { rd(a) };
            let mut line = format!("    {:08x}:", a);
            for (i, byte) in b.iter().enumerate() {
                if i % 4 == 0 {
                    line.push(' ');
                }
                line += &format!("{:02x}", byte);
            }
            logi!("{}", line);
        }
        let Some(next) = a.checked_add(16) else { break };
        a = next;
    }
}

/// The state and callers of the current guest call (diagnostics).
pub fn dump_guest_caller(c: &mut Cpu, why: &str) {
    logi!("{}: r0={:08x} r1={:08x} r2={:08x} r3={:08x} r4={:08x} r5={:08x} r6={:08x} r7={:08x}", why, c.r(0), c.r(1), c.r(2), c.r(3), c.r(4), c.r(5), c.r(6), c.r(7));
    logi!("  r8={:08x} r9={:08x} r10={:08x} r11={:08x} ip={:08x} sp={:08x} lr={:08x}", c.r(8), c.r(9), c.r(10), c.r(11), c.r(12), c.r(13), c.r(14));
    let lr = c.r(14);
    logi!("  called from {} ({})", describe_address(lr & !1), if lr & 1 != 0 { "thumb" } else { "arm" });
    dump_guest_code(lr, 1536, 256);
    // Its callers: code addresses on the guest stack.
    let text = text_ranges();
    let sp = c.r(13);
    let stack_end = c.stack_base as u64 + c.stack_size as u64;
    if sp < c.stack_base || sp as u64 >= stack_end {
        return;
    }
    let mut found = 0;
    let mut a = sp as u64;
    while a + 4 <= stack_end && a < sp as u64 + 1024 && found < 3 {
        let raw: u32 = unsafe { rd(a as u32) };
        let v = raw & !1;
        if text.iter().any(|r| v >= r.0 && v < r.1) && v != (lr & !1) {
            found += 1;
            logi!("  caller (stack sp+0x{:x}): {} ({})", a - sp as u64, describe_address(v), if raw & 1 != 0 { "thumb" } else { "arm" });
            dump_guest_code(v, 512, 128);
        }
        a += 4;
    }
}

/// kind: 0 gettimeofday, 1 clock_gettime, 2 sleep, 3 sched_yield.
pub fn probe_time_call(kind: usize, sleep_us: u64) {
    with_probe(|p| {
        let n = p.calls[kind];
        p.calls[kind] += 1;
        if kind == 2 {
            p.sleep_us_sum += sleep_us;
            p.sleep_count += 1;
            p.sleep_min = p.sleep_min.min(sleep_us);
            p.sleep_max = p.sleep_max.max(sleep_us);
        }
        if n % SAMPLE_EVERY != 0 {
            return;
        }
        let Some(c) = Cpu::current_or_null() else { return };
        let mut s = Site { lr: c.r(14), ..Default::default() };
        // The callers of the calling function: code addresses on the guest stack.
        let sp = c.r(13);
        let stack_end = c.stack_base as u64 + c.stack_size as u64;
        if sp >= c.stack_base && (sp as u64) < stack_end {
            let mut found = 0;
            let mut a = sp as u64;
            while a + 4 <= stack_end && a < sp as u64 + 512 && found < 2 {
                let v: u32 = unsafe { rd(a as u32) };
                a += 4;
                if !p.text.iter().any(|r| (v & !1) >= r.0 && (v & !1) < r.1) || (v & !1) == (s.lr & !1) {
                    continue;
                }
                if found == 0 {
                    s.caller1 = v;
                } else {
                    s.caller2 = v;
                }
                found += 1;
            }
        }
        *p.sites[kind].entry(s).or_insert(0) += 1;
    });
}

pub fn probe_frame() {
    static TOTAL: AtomicU64 = AtomicU64::new(0);
    with_probe(|p| {
        p.frames += 1;
        let total = TOTAL.fetch_add(1, Ordering::Relaxed) + 1;
        if total == REPORT_FRAMES[p.reports] {
            report(p);
        }
    });
}
