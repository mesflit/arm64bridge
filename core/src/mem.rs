//! Guest address space: a region of the host process below 4 GiB that is
//! identity-mapped into the emulated 32-bit CPU (guest address == host address).
// SPDX-License-Identifier: GPL-2.0-or-later
use std::collections::BTreeMap;
use std::os::raw::c_char;
use std::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};
use std::sync::Mutex;

use crate::util::lock;

pub const MAP_FIXED_NOREPLACE: i32 = 0x100000;

static G_BASE: AtomicU32 = AtomicU32::new(0);
static G_SIZE: AtomicU32 = AtomicU32::new(0);
static G_PAGE: AtomicU32 = AtomicU32::new(4096);
// A second, smaller arena below 2 GiB, handed out first. Old code treats pointers as signed numbers
// (Android 2.x's linker copied into games checks "mmap(...) < 0"); on a 32-bit device most addresses
// were below 2 GiB, so they were positive.
static G_LOW_BASE: AtomicU32 = AtomicU32::new(0);
static G_LOW_SIZE: AtomicU32 = AtomicU32::new(0);

#[inline]
fn in_arena(a: u64, len: u64) -> bool {
    let (base, size) = (G_BASE.load(Ordering::Relaxed) as u64, G_SIZE.load(Ordering::Relaxed) as u64);
    if a >= base && a + len <= base + size {
        return true;
    }
    let (lb, ls) = (G_LOW_BASE.load(Ordering::Relaxed) as u64, G_LOW_SIZE.load(Ordering::Relaxed) as u64);
    ls != 0 && a >= lb && a + len <= lb + ls
}

#[derive(Clone, Copy)]
struct Region {
    size: u32,
    exec: bool,
}
struct Pages {
    free: BTreeMap<u32, u32>,    // start -> size
    used: BTreeMap<u32, Region>, // start -> info
}
static PAGES: Mutex<Pages> = Mutex::new(Pages { free: BTreeMap::new(), used: BTreeMap::new() });
static G_EPOCH: AtomicU64 = AtomicU64::new(1);

pub fn mem_arena_base() -> u32 {
    G_BASE.load(Ordering::Relaxed)
}
pub fn mem_arena_size() -> u32 {
    G_SIZE.load(Ordering::Relaxed)
}
/// Second arena below 2 GiB (size 0 if there is none).
pub fn mem_low_arena_base() -> u32 {
    G_LOW_BASE.load(Ordering::Relaxed)
}
pub fn mem_low_arena_size() -> u32 {
    G_LOW_SIZE.load(Ordering::Relaxed)
}
pub fn mem_page_size() -> u32 {
    G_PAGE.load(Ordering::Relaxed)
}

// Guest mappings outside the arena: made when the guest asks for a specific address there (a game's
// own library loader with fixed load addresses). Few, so a small array that readers scan without a lock.
const EXT_MAX: usize = 64;
struct ExtRegion {
    start: AtomicU32,
    size: AtomicU32,
}
#[allow(clippy::declare_interior_mutable_const)]
const EXT_INIT: ExtRegion = ExtRegion { start: AtomicU32::new(0), size: AtomicU32::new(0) };
static G_EXT: [ExtRegion; EXT_MAX] = [EXT_INIT; EXT_MAX];
static G_EXT_COUNT: AtomicI32 = AtomicI32::new(0);

fn ext_index(a: u64) -> i32 {
    let n = G_EXT_COUNT.load(Ordering::Acquire);
    for i in 0..n {
        let st = G_EXT[i as usize].start.load(Ordering::Relaxed);
        let sz = G_EXT[i as usize].size.load(Ordering::Relaxed);
        if sz != 0 && a >= st as u64 && a < st as u64 + sz as u64 {
            return i;
        }
    }
    -1
}

/// True if the host address belongs to guest memory.
#[inline]
pub fn mem_contains(a: u64) -> bool {
    if in_arena(a, 1) {
        return true;
    }
    G_EXT_COUNT.load(Ordering::Relaxed) != 0 && ext_index(a) >= 0
}

/// Changes whenever all translated guest code may have become stale (libraries loaded or unloaded).
pub fn mem_code_epoch() -> u64 {
    G_EPOCH.load(Ordering::Relaxed)
}
pub fn mem_bump_code_epoch() {
    G_EPOCH.fetch_add(1, Ordering::Relaxed);
}

const RANGE_LOG: usize = 1024;
static G_RANGES: Mutex<[(u32, u32); RANGE_LOG]> = Mutex::new([(0, 0); RANGE_LOG]); // ring buffer: entry n % RANGE_LOG is range number n
static G_RANGES_SEQ: AtomicU64 = AtomicU64::new(0);

/// Guest code that changed in a known range: a JIT compiler inside the app (Mono) flushes the
/// instruction cache for every piece of code it generates. Only that range is retranslated.
pub fn mem_invalidate_code(start: u32, len: u32) {
    if len == 0 {
        return;
    }
    let mut r = lock(&G_RANGES);
    let n = G_RANGES_SEQ.load(Ordering::Relaxed);
    r[(n % RANGE_LOG as u64) as usize] = (start, len);
    G_RANGES_SEQ.store(n + 1, Ordering::Release);
}

pub fn mem_code_ranges_seq() -> u64 {
    G_RANGES_SEQ.load(Ordering::Acquire)
}

/// The ranges recorded after `seen`. None if too many were recorded since: then clear everything.
pub fn mem_code_ranges_since(seen: u64) -> Option<Vec<(u32, u32)>> {
    let r = lock(&G_RANGES);
    let now = G_RANGES_SEQ.load(Ordering::Relaxed);
    if now - seen > RANGE_LOG as u64 {
        return None;
    }
    Some((seen..now).map(|n| r[(n % RANGE_LOG as u64) as usize]).collect())
}

/// True if the address lies in pages that are currently allocated (safe to read). Never blocks.
pub fn mem_is_mapped(addr: u32, len: u32) -> bool {
    if !mem_contains(addr as u64) || !mem_contains(addr as u64 + len as u64 - 1) {
        return false;
    }
    if !in_arena(addr as u64, 1) {
        return true; // external mapping
    }
    let Ok(p) = PAGES.try_lock() else { return false };
    match p.used.range(..=addr).next_back() {
        Some((&start, r)) => addr as u64 + len as u64 <= start as u64 + r.size as u64,
        None => false,
    }
}

#[inline]
fn round_up(v: u32, a: u32) -> u32 {
    v.wrapping_add(a - 1) & !(a - 1)
}

unsafe fn reserve(start: u64, size: u64) -> bool {
    let p = libc::mmap(
        start as *mut libc::c_void,
        size as usize,
        libc::PROT_NONE,
        libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE | MAP_FIXED_NOREPLACE,
        -1,
        0,
    );
    if p == libc::MAP_FAILED {
        return false;
    }
    if p as u64 != start {
        // kernel without MAP_FIXED_NOREPLACE treated it as a hint
        libc::munmap(p, size as usize);
        return false;
    }
    true
}

// Reserve up to 256 MiB below 2 GiB (half of a gap at most: ART also allocates there).
fn reserve_low_arena(pages: &mut Pages, gaps: &[(u64, u64)], main_start: u64, main_size: u64) {
    const ALIGN: u64 = 1 << 20;
    const TOP: u64 = 0x8000_0000;
    let mut want: u64 = 256 << 20;
    if let Ok(e) = std::env::var("A64B_LOW_ARENA_MB") {
        want = e.trim().parse::<u64>().unwrap_or(0) << 20;
    }
    if want == 0 {
        return;
    }
    let mut low: Vec<(u64, u64)> = Vec::new();
    for g in gaps {
        let lo = (g.0 + ALIGN - 1) & !(ALIGN - 1);
        let hi = g.1.min(TOP) & !(ALIGN - 1);
        if main_start < hi && main_start + main_size > lo {
            continue;
        }
        if hi > lo + (32 << 20) {
            low.push((lo, hi));
        }
    }
    low.sort_by(|a, b| (b.1 - b.0).cmp(&(a.1 - a.0)));
    for g in &low {
        let size = want.min((g.1 - g.0) / 2) & !(ALIGN - 1);
        let start = g.1 - size;
        if size == 0 || !unsafe { reserve(start, size) } {
            continue;
        }
        G_LOW_BASE.store(start as u32, Ordering::Relaxed);
        G_LOW_SIZE.store(size as u32, Ordering::Relaxed);
        pages.free.insert(start as u32, size as u32);
        logi!("guest arena below 2 GiB: 0x{:08x} - 0x{:08x} ({} MiB), used first", start, start + size, size >> 20);
        return;
    }
    logi!("no room for a guest arena below 2 GiB");
}

pub fn mem_init() -> bool {
    let mut pages = lock(&PAGES);
    if G_BASE.load(Ordering::Relaxed) != 0 {
        return true;
    }
    let ps = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    G_PAGE.store(if ps > 4096 { ps as u32 } else { 4096 }, Ordering::Relaxed);

    // Find the free gaps below 4 GiB.
    let Ok(maps) = std::fs::read_to_string("/proc/self/maps") else {
        loge!("cannot read /proc/self/maps");
        return false;
    };
    const LOW: u64 = 0x1000_0000; // stay away from NULL and the very top
    const HIGH: u64 = 0xFFFF_0000;
    let mut gaps: Vec<(u64, u64)> = Vec::new();
    let mut prev_end = LOW;
    for line in maps.lines() {
        let Some(range) = line.split_whitespace().next() else { continue };
        let Some((s, e)) = range.split_once('-') else { continue };
        let (Ok(s), Ok(e)) = (u64::from_str_radix(s, 16), u64::from_str_radix(e, 16)) else { continue };
        if s >= HIGH {
            break;
        }
        if s > prev_end {
            gaps.push((prev_end, s));
        }
        if e > prev_end {
            prev_end = e;
        }
    }
    if prev_end < HIGH {
        gaps.push((prev_end, HIGH));
    }
    gaps.sort_by(|a, b| (b.1 - b.0).cmp(&(a.1 - a.0)));

    let mut want: u64 = 1024 << 20;
    if let Ok(e) = std::env::var("A64B_ARENA_MB") {
        want = e.trim().parse::<u64>().unwrap_or(0) << 20;
    }
    const ALIGN: u64 = 1 << 20;
    for gap in &gaps {
        let lo = (gap.0 + ALIGN - 1) & !(ALIGN - 1);
        let hi = gap.1 & !(ALIGN - 1);
        if hi <= lo + (64 << 20) {
            continue;
        }
        // Leave room for the runtime (ART also allocates below 4 GiB): never take more than 3/4 of a gap.
        let size = want.min((hi - lo) / 4 * 3) & !(ALIGN - 1);
        // Take the top of the gap; low addresses are what other low-memory users pick first.
        let start = hi - size;
        if size == 0 || !unsafe { reserve(start, size) } {
            continue;
        }
        G_SIZE.store(size as u32, Ordering::Relaxed);
        G_BASE.store(start as u32, Ordering::Relaxed);
        pages.free.insert(start as u32, size as u32);
        logi!("guest arena: 0x{:08x} - 0x{:08x} ({} MiB), host page {}", start, start + size, size >> 20, mem_page_size());
        if start >= 0x8000_0000 {
            reserve_low_arena(&mut pages, &gaps, start, size);
        }
        return true;
    }
    loge!("no usable address space below 4 GiB for the guest arena");
    false
}

fn take_region(p: &mut Pages, size: u32) -> u32 {
    let found = p.free.iter().find(|(_, &sz)| sz >= size).map(|(&s, &sz)| (s, sz));
    match found {
        Some((start, sz)) => {
            p.free.remove(&start);
            if sz > size {
                p.free.insert(start + size, sz - size);
            }
            start
        }
        None => 0,
    }
}

fn give_region(p: &mut Pages, mut start: u32, mut size: u32) {
    if let Some((&ps, &psz)) = p.free.range(..start).next_back() {
        if ps + psz == start {
            start = ps;
            size += psz;
            p.free.remove(&ps);
        }
    }
    if let Some(&nsz) = p.free.get(&start.wrapping_add(size)) {
        p.free.remove(&start.wrapping_add(size));
        size += nsz;
    }
    p.free.insert(start, size);
}

/// Page level allocation (always zero filled, read/write on the host side).
pub fn page_alloc(size: u32, exec: bool) -> u32 {
    if size == 0 {
        return 0;
    }
    let size = round_up(size, mem_page_size());
    let mut p = lock(&PAGES);
    let a = take_region(&mut p, size);
    if a == 0 {
        loge!("guest arena exhausted (request {} bytes)", size);
        return 0;
    }
    if unsafe { libc::mprotect(a as usize as *mut _, size as usize, libc::PROT_READ | libc::PROT_WRITE) } != 0 {
        loge!("mprotect failed for guest pages");
        give_region(&mut p, a, size);
        return 0;
    }
    p.used.insert(a, Region { size, exec });
    a
}

/// Map a file (or anonymous memory if fd < 0) at a guest address chosen by us.
pub fn page_map_file(size: u32, host_prot: i32, host_flags: i32, fd: i32, offset: i64) -> u32 {
    if size == 0 {
        return 0;
    }
    let size = round_up(size, mem_page_size());
    let mut p = lock(&PAGES);
    let a = take_region(&mut p, size);
    if a == 0 {
        return 0;
    }
    let flags = (host_flags & !MAP_FIXED_NOREPLACE) | libc::MAP_FIXED;
    // The host never executes guest code, and the emulator needs read access.
    let prot = (host_prot & libc::PROT_WRITE) | libc::PROT_READ;
    let m = unsafe { libc::mmap(a as usize as *mut _, size as usize, prot, flags, fd, offset as libc::off_t) };
    if m == libc::MAP_FAILED {
        give_region(&mut p, a, size);
        return 0;
    }
    p.used.insert(a, Region { size, exec: (host_prot & libc::PROT_EXEC) != 0 });
    a
}

// True if [addr, addr + size) is completely inside allocated arena regions.
fn covered_by_used(p: &Pages, addr: u32, size: u32) -> bool {
    let (mut pos, end) = (addr as u64, addr as u64 + size as u64);
    while pos < end {
        let Some((&s, r)) = p.used.range(..=(pos as u32)).next_back() else { return false };
        let rend = s as u64 + r.size as u64;
        if pos >= rend {
            return false;
        }
        pos = rend;
    }
    true
}

unsafe fn map_fixed(addr: u32, size: u32, prot: i32, flags: i32, fd: i32, offset: i64) -> bool {
    libc::mmap(addr as usize as *mut _, size as usize, prot, flags, fd, offset as libc::off_t) != libc::MAP_FAILED
}

/// Map at the guest address the guest asked for (anonymous if fd < 0). With `fixed` an existing guest
/// mapping there is replaced (MAP_FIXED); without it the address is only used when it is free.
/// Addresses outside the arena are used when the host has nothing there. Returns 0 if not possible.
pub fn page_map_at(addr: u32, size: u32, host_prot: i32, host_flags: i32, fd: i32, offset: i64, fixed: bool) -> u32 {
    let page = mem_page_size();
    let size = round_up(size, page);
    if addr == 0 || size == 0 || (addr & (page - 1)) != 0 || addr as u64 + size as u64 > 0xFFFF_0000 {
        return 0;
    }
    let exec = (host_prot & libc::PROT_EXEC) != 0;
    let prot = (host_prot & libc::PROT_WRITE) | libc::PROT_READ; // the host never executes guest code
    let ty = if (host_flags & libc::MAP_SHARED) != 0 { libc::MAP_SHARED } else { libc::MAP_PRIVATE };
    let anon = if fd < 0 { libc::MAP_ANONYMOUS } else { 0 };
    let mut guard = lock(&PAGES);
    let p = &mut *guard;
    unsafe {
        if in_arena(addr as u64, size as u64) {
            if fixed && !covered_by_used(p, addr, size) {
                // MAP_FIXED over a range that is partly free (a loader extending a segment past its own
                // reservation): like Linux, map over all of it; the free parts become allocated.
                let end = addr as u64 + size as u64;
                let overlapping: Vec<(u32, u32)> =
                    p.free.iter().filter(|(&fs, &fsz)| fs as u64 + fsz as u64 > addr as u64 && (fs as u64) < end).map(|(&a, &b)| (a, b)).collect();
                for (fs, fsz) in overlapping {
                    let (fs64, fe) = (fs as u64, fs as u64 + fsz as u64);
                    let (cs, ce) = (fs64.max(addr as u64), fe.min(end));
                    p.free.remove(&fs);
                    if fs64 < cs {
                        p.free.insert(fs, (cs - fs64) as u32);
                    }
                    if ce < fe {
                        p.free.insert(ce as u32, (fe - ce) as u32);
                    }
                    p.used.insert(cs as u32, Region { size: (ce - cs) as u32, exec });
                }
                if !map_fixed(addr, size, prot, ty | anon | libc::MAP_FIXED, fd, offset) {
                    return 0;
                }
                if exec {
                    mem_bump_code_epoch();
                }
                return addr;
            }
            if covered_by_used(p, addr, size) {
                if !fixed {
                    return 0; // a hint for memory that is in use
                }
                if !map_fixed(addr, size, prot, ty | anon | libc::MAP_FIXED, fd, offset) {
                    return 0;
                }
                if exec {
                    if let Some((_, r)) = p.used.range_mut(..=addr).next_back() {
                        r.exec = true;
                    }
                    mem_bump_code_epoch();
                }
                return addr;
            }
            // Completely inside one free region: carve it out.
            let Some((&fs, &fsz)) = p.free.range(..=addr).next_back() else { return 0 };
            if addr as u64 + size as u64 > fs as u64 + fsz as u64 {
                return 0;
            }
            p.free.remove(&fs);
            if addr > fs {
                p.free.insert(fs, addr - fs);
            }
            if (addr as u64 + size as u64) < fs as u64 + fsz as u64 {
                p.free.insert(addr + size, fs + fsz - (addr + size));
            }
            let ok = if fd < 0 {
                libc::mprotect(addr as usize as *mut _, size as usize, libc::PROT_READ | libc::PROT_WRITE) == 0
            } else {
                map_fixed(addr, size, prot, ty | libc::MAP_FIXED, fd, offset)
            };
            if !ok {
                give_region(p, addr, size);
                return 0;
            }
            p.used.insert(addr, Region { size, exec });
            return addr;
        }
        let (base, asz) = (mem_arena_base() as u64, mem_arena_size() as u64);
        let (lb, ls) = (mem_low_arena_base() as u64, mem_low_arena_size() as u64);
        let (a64, e64) = (addr as u64, addr as u64 + size as u64);
        if (e64 > base && a64 < base + asz) || (ls != 0 && e64 > lb && a64 < lb + ls) {
            return 0; // straddles an arena edge
        }
        // Outside the arena.
        let e = ext_index(a64);
        if e >= 0 {
            let st = G_EXT[e as usize].start.load(Ordering::SeqCst) as u64;
            let sz = G_EXT[e as usize].size.load(Ordering::SeqCst) as u64;
            if !fixed || e64 > st + sz {
                return 0;
            }
            if !map_fixed(addr, size, prot, ty | anon | libc::MAP_FIXED, fd, offset) {
                return 0;
            }
            mem_bump_code_epoch();
            return addr;
        }
        let n = G_EXT_COUNT.load(Ordering::SeqCst);
        if n as usize >= EXT_MAX {
            return 0;
        }
        let m = libc::mmap(addr as usize as *mut _, size as usize, prot, ty | anon | MAP_FIXED_NOREPLACE, fd, offset as libc::off_t);
        if m == libc::MAP_FAILED {
            return 0;
        }
        if m as usize != addr as usize {
            // kernel without MAP_FIXED_NOREPLACE treated it as a hint
            libc::munmap(m, size as usize);
            return 0;
        }
        G_EXT[n as usize].start.store(addr, Ordering::SeqCst);
        G_EXT[n as usize].size.store(size, Ordering::SeqCst);
        G_EXT_COUNT.store(n + 1, Ordering::Release);
        if exec {
            mem_bump_code_epoch();
        }
        addr
    }
}

unsafe fn drop_pages(addr: u32, size: u32) {
    libc::mmap(
        addr as usize as *mut _,
        size as usize,
        libc::PROT_NONE,
        libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE | libc::MAP_FIXED,
        -1,
        0,
    );
}

pub fn page_free(addr: u32, size: u32) -> bool {
    let page = mem_page_size();
    let mut size = round_up(size, page);
    if !mem_contains(addr as u64) || size == 0 || (addr & (page - 1)) != 0 {
        return false;
    }
    let mut guard = lock(&PAGES);
    let p = &mut *guard;
    if !in_arena(addr as u64, 1) {
        // External mapping: give the pages back; the range stays reserved for the guest.
        let e = ext_index(addr as u64);
        if e < 0 {
            return false;
        }
        let st = G_EXT[e as usize].start.load(Ordering::SeqCst);
        let sz = G_EXT[e as usize].size.load(Ordering::SeqCst);
        if addr as u64 + size as u64 > st as u64 + sz as u64 {
            size = st + sz - addr;
        }
        unsafe { drop_pages(addr, size) };
        return true;
    }
    // Find the region containing addr; partial unmaps split the bookkeeping.
    let Some((&rs, &r)) = p.used.range(..=addr).next_back() else { return false };
    let (rsz, exec) = (r.size, r.exec);
    let rend = rs as u64 + rsz as u64;
    if addr as u64 >= rend {
        return false;
    }
    if addr as u64 + size as u64 > rend {
        size = (rend - addr as u64) as u32;
    }
    p.used.remove(&rs);
    if addr > rs {
        p.used.insert(rs, Region { size: addr - rs, exec });
    }
    if (addr as u64 + size as u64) < rend {
        p.used.insert(addr + size, Region { size: (rend - (addr as u64 + size as u64)) as u32, exec });
    }
    // Drop the pages and return to the reserved state.
    unsafe { drop_pages(addr, size) };
    give_region(p, addr, size);
    if exec {
        mem_bump_code_epoch();
    }
    true
}

/// Allocated arena regions (start, size).
pub fn mem_used_regions() -> Vec<(u32, u32)> {
    lock(&PAGES).used.iter().map(|(&s, r)| (s, r.size)).collect()
}

pub fn page_mark_exec(addr: u32, _size: u32) {
    let mut p = lock(&PAGES);
    if let Some((&s, r)) = p.used.range_mut(..=addr).next_back() {
        if (addr as u64) < s as u64 + r.size as u64 {
            r.exec = true;
        }
    }
}

/// Host pointer of a guest address.
#[inline(always)]
pub fn g2h<T>(addr: u32) -> *mut T {
    addr as usize as *mut T
}

/// Convert a host pointer to a guest address. Pointers outside the low 4 GiB
/// cannot be represented; they are reported and become NULL.
#[inline]
pub fn h2g<T>(p: *const T) -> u32 {
    let v = p as usize as u64;
    if v > 0xFFFF_FFFF {
        loge!("host pointer {:p} is not representable in the 32-bit guest", p);
        return 0;
    }
    v as u32
}

/// Reads a (possibly unaligned) value from guest memory.
#[inline(always)]
pub unsafe fn rd<T: Copy>(addr: u32) -> T {
    std::ptr::read_unaligned(addr as usize as *const T)
}
/// Writes a (possibly unaligned) value to guest memory.
#[inline(always)]
pub unsafe fn wr<T>(addr: u32, v: T) {
    std::ptr::write_unaligned(addr as usize as *mut T, v)
}

// ---------------------------------------------------------------------------
// malloc: size classes carved from slabs, large blocks straight from pages.
// Every block has an 8 byte header: { u32 tag; u32 value }.
// Large blocks get one mapped page before and after them: on a real device the heap around a block
// is mapped too, and games do read a little past the ends of their buffers (Minion Rush reads the
// 4 bytes before one while copying); here that would hit an unmapped page and crash.
// ---------------------------------------------------------------------------
const TAG_SMALL: u32 = 0xA64B_5000; // | class index
const TAG_LARGE: u32 = 0xA64B_1A00; // value = total mapped size
const TAG_ALIGNED: u32 = 0xA64B_A100; // value = original user pointer
const TAG_FREE: u32 = 0xA64B_F4EE;
const CLASS_SIZES: [u32; 22] =
    [16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768];
const NUM_CLASSES: usize = CLASS_SIZES.len();
const SLAB: u32 = 256 * 1024;

static FREELIST: Mutex<[u32; NUM_CLASSES]> = Mutex::new([0; NUM_CLASSES]);

fn class_for(size: u32) -> Option<usize> {
    CLASS_SIZES.iter().position(|&c| size <= c)
}

#[inline]
unsafe fn hdr_tag(ptr: u32) -> *mut u32 {
    g2h::<u32>(ptr - 8)
}
#[inline]
unsafe fn hdr_value(ptr: u32) -> *mut u32 {
    g2h::<u32>(ptr - 4)
}

pub fn g_malloc(size: u32) -> u32 {
    let size = if size == 0 { 1 } else { size };
    let page = mem_page_size();
    unsafe {
        let Some(c) = class_for(size) else {
            if size > 0xF000_0000 {
                return 0;
            }
            let total = round_up(size, page) + 2 * page;
            let a = page_alloc(total, false);
            if a == 0 {
                return 0;
            }
            let user = a + page;
            *hdr_tag(user) = TAG_LARGE;
            *hdr_value(user) = total;
            return user;
        };
        let mut fl = lock(&FREELIST);
        if fl[c] == 0 {
            let cell = CLASS_SIZES[c] + 8;
            // Slabs are padded like large blocks (see above).
            let mut slab = page_alloc(SLAB + 2 * page, false);
            if slab == 0 {
                return 0;
            }
            slab += page;
            for i in 0..SLAB / cell {
                let user = slab + i * cell + 8;
                *g2h::<u32>(user) = fl[c];
                fl[c] = user;
            }
        }
        let user = fl[c];
        fl[c] = *g2h::<u32>(user);
        *hdr_tag(user) = TAG_SMALL | c as u32;
        *hdr_value(user) = size;
        user
    }
}

pub fn g_free(ptr: u32) {
    if ptr == 0 {
        return;
    }
    if !mem_contains(ptr as u64) || (ptr & 7) != 0 {
        logw!("free(0x{:08x}): not a guest heap pointer, ignored", ptr);
        return;
    }
    unsafe {
        let tag = *hdr_tag(ptr);
        if tag == TAG_ALIGNED {
            let orig = *hdr_value(ptr);
            *hdr_tag(ptr) = TAG_FREE;
            g_free(orig);
            return;
        }
        if tag == TAG_LARGE {
            let total = *hdr_value(ptr);
            *hdr_tag(ptr) = TAG_FREE;
            page_free(ptr - mem_page_size(), total);
            return;
        }
        if (tag & 0xFFFF_FF00) == TAG_SMALL {
            let c = (tag & 0xFF) as usize;
            if c >= NUM_CLASSES {
                return;
            }
            let mut fl = lock(&FREELIST);
            *hdr_tag(ptr) = TAG_FREE;
            *g2h::<u32>(ptr) = fl[c];
            fl[c] = ptr;
            return;
        }
    }
    logw!("free(0x{:08x}): bad or double free, ignored", ptr);
}

pub fn g_usable_size(ptr: u32) -> u32 {
    if ptr == 0 || !mem_contains(ptr as u64) {
        return 0;
    }
    unsafe {
        let (tag, value) = (*hdr_tag(ptr), *hdr_value(ptr));
        if tag == TAG_LARGE {
            return value - 2 * mem_page_size();
        }
        if (tag & 0xFFFF_FF00) == TAG_SMALL && ((tag & 0xFF) as usize) < NUM_CLASSES {
            return CLASS_SIZES[(tag & 0xFF) as usize];
        }
        if tag == TAG_ALIGNED {
            return g_usable_size(value).wrapping_sub(ptr - value);
        }
    }
    0
}

pub fn g_calloc(n: u32, size: u32) -> u32 {
    let total = n as u64 * size as u64;
    if total > 0xF000_0000 {
        return 0;
    }
    let p = g_malloc(total as u32);
    if p != 0 {
        unsafe { std::ptr::write_bytes(g2h::<u8>(p), 0, total as usize) };
    }
    p
}

pub fn g_realloc(ptr: u32, size: u32) -> u32 {
    if ptr == 0 {
        return g_malloc(size);
    }
    if size == 0 {
        g_free(ptr);
        return 0;
    }
    let old = g_usable_size(ptr);
    if old >= size && (old <= 64 || size >= old / 2) {
        return ptr;
    }
    let n = g_malloc(size);
    if n == 0 {
        return 0;
    }
    unsafe { std::ptr::copy_nonoverlapping(g2h::<u8>(ptr), g2h::<u8>(n), old.min(size) as usize) };
    g_free(ptr);
    n
}

pub fn g_memalign(align: u32, size: u32) -> u32 {
    if align <= 8 {
        return g_malloc(size);
    }
    if (align & (align - 1)) != 0 {
        return 0;
    }
    let raw = g_malloc(size.wrapping_add(align).wrapping_add(8));
    if raw == 0 {
        return 0;
    }
    let user = (raw + 8 + align - 1) & !(align - 1);
    unsafe {
        *hdr_tag(user) = TAG_ALIGNED;
        *hdr_value(user) = raw;
    }
    user
}

/// Copies a host C string into guest memory.
pub unsafe fn g_strdup(s: *const c_char) -> u32 {
    if s.is_null() {
        return 0;
    }
    let n = libc::strlen(s) + 1;
    let p = g_malloc(n as u32);
    if p != 0 {
        std::ptr::copy_nonoverlapping(s as *const u8, g2h::<u8>(p), n);
    }
    p
}

/// Copies bytes into guest memory, adding a terminating NUL.
pub fn g_strdup_bytes(s: &[u8]) -> u32 {
    let p = g_malloc(s.len() as u32 + 1);
    if p != 0 {
        unsafe {
            std::ptr::copy_nonoverlapping(s.as_ptr(), g2h::<u8>(p), s.len());
            *g2h::<u8>(p + s.len() as u32) = 0;
        }
    }
    p
}
