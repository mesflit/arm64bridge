//! Loader for 32-bit ARM shared objects.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use crate::cpu::{trap_name_at, Cpu};
use crate::mem::*;
use crate::thunk::{thunk_missing, thunk_register_data, thunk_resolve, thunks_init};
use crate::util::{base_name, lock, RecMutex};

#[repr(C)]
#[derive(Clone, Copy)]
struct Ehdr {
    e_ident: [u8; 16],
    e_type: u16,
    e_machine: u16,
    e_version: u32,
    e_entry: u32,
    e_phoff: u32,
    e_shoff: u32,
    e_flags: u32,
    e_ehsize: u16,
    e_phentsize: u16,
    e_phnum: u16,
    e_shentsize: u16,
    e_shnum: u16,
    e_shstrndx: u16,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Phdr {
    p_type: u32,
    p_offset: u32,
    p_vaddr: u32,
    p_paddr: u32,
    p_filesz: u32,
    p_memsz: u32,
    p_flags: u32,
    p_align: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct ElfDyn {
    d_tag: i32,
    d_val: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Sym {
    st_name: u32,
    st_value: u32,
    st_size: u32,
    st_info: u8,
    st_other: u8,
    st_shndx: u16,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Rel {
    r_offset: u32,
    r_info: u32,
}

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_TLS: u32 = 7;
const PT_ARM_EXIDX: u32 = 0x7000_0001;
const EM_386: u16 = 3;
const EM_ARM: u16 = 40;
const EM_X86_64: u16 = 62;
const EM_AARCH64: u16 = 183;
const ET_DYN: u16 = 3;

const DT_NULL: u32 = 0;
const DT_NEEDED: u32 = 1;
const DT_PLTRELSZ: u32 = 2;
const DT_HASH: u32 = 4;
const DT_STRTAB: u32 = 5;
const DT_SYMTAB: u32 = 6;
const DT_STRSZ: u32 = 10;
const DT_INIT: u32 = 12;
const DT_SYMBOLIC: u32 = 16;
const DT_REL: u32 = 17;
const DT_RELSZ: u32 = 18;
const DT_PLTREL: u32 = 20;
const DT_JMPREL: u32 = 23;
const DT_INIT_ARRAY: u32 = 25;
const DT_INIT_ARRAYSZ: u32 = 27;
const DT_FLAGS: u32 = 30;
const DT_RELRSZ: u32 = 35;
const DT_RELR: u32 = 36;
const DT_GNU_HASH: u32 = 0x6fff_fef5;
const A_DT_ANDROID_REL: u32 = 0x6000_000f;
const A_DT_ANDROID_RELSZ: u32 = 0x6000_0010;
const A_DT_ANDROID_RELR: u32 = 0x6fff_e000;
const A_DT_ANDROID_RELRSZ: u32 = 0x6fff_e001;
const DF_SYMBOLIC: u32 = 2;

const STB_GLOBAL: u8 = 1;
const STB_WEAK: u8 = 2;
const STT_OBJECT: u8 = 1;

const R_ARM_NONE: u32 = 0;
const R_ARM_ABS32: u32 = 2;
const R_ARM_REL32: u32 = 3;
const R_ARM_TLS_DTPMOD32: u32 = 17;
const R_ARM_TLS_DTPOFF32: u32 = 18;
const R_ARM_TLS_TPOFF32: u32 = 19;
const R_ARM_GLOB_DAT: u32 = 21;
const R_ARM_JUMP_SLOT: u32 = 22;
const R_ARM_RELATIVE: u32 = 23;
const R_ARM_IRELATIVE: u32 = 160;

/// A loaded guest library. Modules live until the process ends.
#[derive(Default)]
pub struct Module {
    pub name: String,
    pub base: u32, // lowest mapped address
    pub size: u32,
    pub bias: u32, // load bias (address = bias + vaddr)
    pub phdr: u32, // guest address of the program headers
    pub phnum: u32,
    pub exidx: u32, // ARM exception index table
    pub exidx_count: u32,
    pub init_func: u32,
    pub init_array: u32,
    pub init_array_count: u32,
    pub symbolic: bool,
    pub constructed: AtomicBool,
    pub exports: HashMap<String, u32>, // name -> guest address
    pub needed: Vec<String>,
}

impl Module {
    pub fn find(&self, sym: &str) -> u32 {
        self.exports.get(sym).copied().unwrap_or(0)
    }
}

pub type ModRef = &'static Module;

/// Supplies the bytes of a guest library by file name ("libfoo.so").
pub type LibProvider = Box<dyn Fn(&str) -> Option<Vec<u8>> + Send + Sync>;

static LOAD_MU: RecMutex = RecMutex::new();
static MODULES: Mutex<Vec<ModRef>> = Mutex::new(Vec::new());
static PROVIDERS: Mutex<Vec<&'static LibProvider>> = Mutex::new(Vec::new());

fn modules_snapshot() -> Vec<ModRef> {
    lock(&MODULES).clone()
}

// A view of an ELF image laid out by virtual address. "origin" is the host
// address that corresponds to vaddr 0 (may point before the buffer).
#[derive(Default)]
struct DynView {
    origin: usize,
    lo: u32,
    hi: u32, // valid vaddr range
    symtab: usize,
    strtab: usize,
    strsz: u32,
    nsyms: u32,
    rel: usize,
    relsz: u32,
    jmprel: usize,
    jmprelsz: u32,
    relr: usize,
    relrsz: u32,
    android_rel: usize,
    android_relsz: u32,
    init: u32,
    init_array: u32,
    init_arraysz: u32,
    symbolic: bool,
    needed_off: Vec<u32>,
}

impl DynView {
    /// Host address of `len` bytes at vaddr, or 0 if out of range.
    fn at(&self, vaddr: u32, len: u64) -> usize {
        if vaddr < self.lo || vaddr as u64 + len > self.hi as u64 {
            return 0;
        }
        self.origin.wrapping_add(vaddr as usize)
    }
    unsafe fn word(&self, vaddr: u32) -> Option<u32> {
        let p = self.at(vaddr, 4);
        (p != 0).then(|| std::ptr::read_unaligned(p as *const u32))
    }
    unsafe fn str(&self, off: u32) -> String {
        if self.strtab == 0 || off >= self.strsz {
            return String::new();
        }
        let start = (self.strtab + off as usize) as *const u8;
        let max = (self.strsz - off) as usize;
        let mut n = 0;
        while n < max && *start.add(n) != 0 {
            n += 1;
        }
        String::from_utf8_lossy(std::slice::from_raw_parts(start, n)).into_owned()
    }
    unsafe fn sym(&self, i: u32) -> Sym {
        std::ptr::read_unaligned((self.symtab + i as usize * std::mem::size_of::<Sym>()) as *const Sym)
    }
}

unsafe fn ehdr(data: &[u8]) -> Ehdr {
    std::ptr::read_unaligned(data.as_ptr() as *const Ehdr)
}
unsafe fn phdr(data: &[u8], eh: &Ehdr, i: usize) -> Phdr {
    std::ptr::read_unaligned(data.as_ptr().add(eh.e_phoff as usize + i * std::mem::size_of::<Phdr>()) as *const Phdr)
}

/// Checks that the file is a 32-bit ARM shared object. `machine` receives a description of what it is.
fn check_header(data: &[u8], machine: Option<&mut String>) -> Result<(), String> {
    if data.len() < std::mem::size_of::<Ehdr>() || &data[..4] != b"\x7fELF" {
        return Err("not an ELF file".into());
    }
    let eh = unsafe { ehdr(data) };
    if let Some(m) = machine {
        *m = if data[4] == 2 {
            let em = u16::from_le_bytes([data[18], data[19]]);
            if em == EM_AARCH64 { "arm64" } else if em == EM_X86_64 { "x86_64" } else { "64-bit" }
        } else if eh.e_machine == EM_ARM {
            "arm"
        } else if eh.e_machine == EM_386 {
            "x86"
        } else {
            "32-bit"
        }
        .to_string();
    }
    if data[4] != 1 || data[5] != 1 || eh.e_machine != EM_ARM {
        return Err("not a 32-bit ARM library".into());
    }
    if eh.e_type != ET_DYN {
        return Err("not a shared object".into());
    }
    if eh.e_phentsize as usize != std::mem::size_of::<Phdr>() || eh.e_phoff as u64 + eh.e_phnum as u64 * std::mem::size_of::<Phdr>() as u64 > data.len() as u64 {
        return Err("corrupt program headers".into());
    }
    Ok(())
}

unsafe fn parse_dynamic(d: &mut DynView, dyn_vaddr: u32, dyn_size: u32) -> Result<(), String> {
    let n = dyn_size as usize / std::mem::size_of::<ElfDyn>();
    let dynp = d.at(dyn_vaddr, dyn_size as u64);
    if dynp == 0 {
        return Err("dynamic section out of range".into());
    }
    let (mut symtab, mut strtab, mut hash, mut gnu_hash, mut rel, mut jmprel, mut relr, mut arel) = (0u32, 0u32, 0u32, 0u32, 0u32, 0u32, 0u32, 0u32);
    for i in 0..n {
        let e = std::ptr::read_unaligned((dynp + i * std::mem::size_of::<ElfDyn>()) as *const ElfDyn);
        let (tag, v) = (e.d_tag as u32, e.d_val);
        match tag {
            DT_NULL => break,
            DT_SYMTAB => symtab = v,
            DT_STRTAB => strtab = v,
            DT_STRSZ => d.strsz = v,
            DT_HASH => hash = v,
            DT_GNU_HASH => gnu_hash = v,
            DT_REL => rel = v,
            DT_RELSZ => d.relsz = v,
            DT_JMPREL => jmprel = v,
            DT_PLTRELSZ => d.jmprelsz = v,
            DT_PLTREL => {
                if v != DT_REL {
                    return Err("RELA PLT relocations are not valid for ARM".into());
                }
            }
            DT_RELR | A_DT_ANDROID_RELR => relr = v,
            DT_RELRSZ | A_DT_ANDROID_RELRSZ => d.relrsz = v,
            A_DT_ANDROID_REL => arel = v,
            A_DT_ANDROID_RELSZ => d.android_relsz = v,
            DT_INIT => d.init = v,
            DT_INIT_ARRAY => d.init_array = v,
            DT_INIT_ARRAYSZ => d.init_arraysz = v,
            DT_SYMBOLIC => d.symbolic = true,
            DT_FLAGS => {
                if v & DF_SYMBOLIC != 0 {
                    d.symbolic = true;
                }
            }
            DT_NEEDED => d.needed_off.push(v),
            _ => {}
        }
    }
    d.strtab = d.at(strtab, d.strsz as u64);
    if d.strtab == 0 || symtab == 0 {
        return Err("missing dynamic symbol/string table".into());
    }
    if hash != 0 {
        if d.at(hash, 8) != 0 {
            d.nsyms = d.word(hash + 4).unwrap_or(0);
        }
    } else if gnu_hash != 0 && d.at(gnu_hash, 16) != 0 {
        let nbuckets = d.word(gnu_hash).unwrap_or(0);
        let symoffset = d.word(gnu_hash + 4).unwrap_or(0);
        let bloom = d.word(gnu_hash + 8).unwrap_or(0);
        let buckets = gnu_hash.wrapping_add(16).wrapping_add(bloom.wrapping_mul(4));
        let mut maxsym = 0;
        if d.at(buckets, nbuckets as u64 * 4) != 0 {
            for i in 0..nbuckets {
                maxsym = maxsym.max(d.word(buckets + i * 4).unwrap_or(0));
            }
        }
        if maxsym < symoffset {
            d.nsyms = symoffset;
        } else {
            let chains = buckets.wrapping_add(nbuckets.wrapping_mul(4));
            let mut i = maxsym;
            loop {
                match d.word(chains.wrapping_add((i - symoffset).wrapping_mul(4))) {
                    None => break,
                    Some(ch) if ch & 1 != 0 => break,
                    _ => i += 1,
                }
            }
            d.nsyms = i + 1;
        }
    }
    d.symtab = d.at(symtab, d.nsyms as u64 * std::mem::size_of::<Sym>() as u64);
    if d.symtab == 0 {
        return Err("symbol table out of range".into());
    }
    if rel != 0 {
        d.rel = d.at(rel, d.relsz as u64);
    }
    if jmprel != 0 {
        d.jmprel = d.at(jmprel, d.jmprelsz as u64);
    }
    if relr != 0 {
        d.relr = d.at(relr, d.relrsz as u64);
    }
    if arel != 0 {
        d.android_rel = d.at(arel, d.android_relsz as u64);
    }
    Ok(())
}

struct SlebReader<'a> {
    d: &'a [u8],
    p: usize,
    overrun: bool,
}
impl SlebReader<'_> {
    fn next(&mut self) -> i32 {
        let mut value: i32 = 0;
        let mut shift = 0u32;
        loop {
            if self.p >= self.d.len() {
                self.overrun = true;
                return 0;
            }
            let b = self.d[self.p];
            self.p += 1;
            if shift < 32 {
                value |= (((b & 0x7f) as u32) << shift) as i32;
            }
            shift += 7;
            if b & 0x80 == 0 {
                if shift < 32 && (b & 0x40) != 0 {
                    value |= (1u32 << shift).wrapping_neg() as i32;
                }
                return value;
            }
        }
    }
}

// Decodes Android packed relocations (APS2) into plain REL entries.
fn unpack_android_rel(data: &[u8]) -> Option<Vec<Rel>> {
    if data.len() < 4 || &data[..4] != b"APS2" {
        return None;
    }
    let mut r = SlebReader { d: &data[4..], p: 0, overrun: false };
    let count = r.next() as u32 as usize;
    let mut offset = r.next() as u32;
    let mut info = 0u32;
    let mut out = Vec::with_capacity(count.min(1 << 20));
    while out.len() < count {
        let group_size = r.next() as u32;
        let flags = r.next() as u32;
        let (by_info, by_delta, by_addend, has_addend) = (flags & 1 != 0, flags & 2 != 0, flags & 4 != 0, flags & 8 != 0);
        let delta = if by_delta { r.next() as u32 } else { 0 };
        if by_info {
            info = r.next() as u32;
        }
        if has_addend && by_addend {
            r.next();
        }
        if group_size == 0 || r.overrun {
            return None;
        }
        let mut i = 0;
        while i < group_size && out.len() < count {
            offset = offset.wrapping_add(if by_delta { delta } else { r.next() as u32 });
            if !by_info {
                info = r.next() as u32;
            }
            if has_addend && !by_addend {
                r.next();
            }
            out.push(Rel { r_offset: offset, r_info: info });
            i += 1;
        }
    }
    Some(out)
}

const SYSTEM_LIBS: [&str; 19] = [
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
    "libz.so", "libOpenSLES.so", "libjnigraphics.so", "libOpenMAXAL.so", "libmediandk.so", "libvulkan.so", "libcamera2ndk.so", "libaaudio.so",
    "libnativewindow.so",
];

fn fetch(name: &str) -> Option<Vec<u8>> {
    let providers: Vec<&'static LibProvider> = lock(&PROVIDERS).clone();
    providers.iter().find_map(|p| p(name))
}

unsafe fn resolve_symbol(this: &Module, d: &DynView, symidx: u32, weak_undef: &mut bool) -> u32 {
    *weak_undef = false;
    if symidx == 0 || symidx >= d.nsyms {
        return 0;
    }
    let s = d.sym(symidx);
    if s.st_shndx != 0 {
        return this.bias.wrapping_add(s.st_value);
    }
    let name = d.str(s.st_name);
    for m in modules_snapshot() {
        if std::ptr::eq(m, this) {
            continue;
        }
        let a = m.find(&name);
        if a != 0 {
            return a;
        }
    }
    let a = thunk_resolve(&name);
    if a != 0 {
        return a;
    }
    if s.st_info >> 4 == STB_WEAK {
        *weak_undef = true;
        return 0;
    }
    if s.st_info & 0xf == STT_OBJECT {
        logw!("{}: unresolved data symbol {} (zero-filled placeholder)", this.name, name);
        let a = g_calloc(1, s.st_size.max(64));
        thunk_register_data(&name, a);
        return a;
    }
    logw!("{}: unresolved function {}", this.name, name);
    thunk_missing(&name)
}

unsafe fn apply_rel(m: &Module, d: &DynView, rel: *const Rel, count: u32) -> Result<(), String> {
    for i in 0..count as usize {
        let r = std::ptr::read_unaligned(rel.add(i));
        let (ty, sym) = (r.r_info & 0xff, r.r_info >> 8);
        let where_ = m.bias.wrapping_add(r.r_offset);
        if where_ < m.base || where_ as u64 + 4 > m.base as u64 + m.size as u64 {
            return Err("relocation outside of image".into());
        }
        let cur: u32 = rd(where_);
        let mut weak = false;
        match ty {
            R_ARM_NONE => {}
            R_ARM_RELATIVE => wr(where_, cur.wrapping_add(m.bias)),
            R_ARM_ABS32 => wr(where_, cur.wrapping_add(resolve_symbol(m, d, sym, &mut weak))),
            R_ARM_REL32 => wr(where_, cur.wrapping_add(resolve_symbol(m, d, sym, &mut weak)).wrapping_sub(where_)),
            R_ARM_GLOB_DAT | R_ARM_JUMP_SLOT => wr(where_, resolve_symbol(m, d, sym, &mut weak)),
            R_ARM_IRELATIVE => wr(where_, Cpu::current().call(m.bias.wrapping_add(cur), &[]) as u32),
            R_ARM_TLS_DTPMOD32 | R_ARM_TLS_DTPOFF32 | R_ARM_TLS_TPOFF32 => logw!("{}: ELF TLS relocation {} is not supported", m.name, ty),
            _ => logw!("{}: unknown relocation type {}", m.name, ty),
        }
    }
    Ok(())
}

unsafe fn load_locked(name: &str) -> Result<ModRef, String> {
    if let Some(m) = modules_snapshot().into_iter().find(|m| m.name == name) {
        return Ok(m);
    }
    let Some(file) = fetch(name) else { return Err(format!("library {} not found", name)) };
    check_header(&file, None).map_err(|e| format!("{}: {}", name, e))?;
    let eh = ehdr(&file);
    let (mut lo, mut hi) = (0xFFFF_FFFFu32, 0u32);
    for i in 0..eh.e_phnum as usize {
        let p = phdr(&file, &eh, i);
        if p.p_type != PT_LOAD {
            continue;
        }
        if p.p_offset as u64 + p.p_filesz as u64 > file.len() as u64 || p.p_filesz > p.p_memsz {
            return Err(format!("{}: corrupt segment", name));
        }
        lo = lo.min(p.p_vaddr & !0xFFF);
        hi = hi.max(p.p_vaddr.wrapping_add(p.p_memsz).wrapping_add(0xFFF) & !0xFFF);
    }
    if hi <= lo {
        return Err(format!("{}: no loadable segments", name));
    }
    let base = page_alloc(hi - lo, true);
    if base == 0 {
        return Err(format!("{}: out of guest memory", name));
    }
    let mut m = Box::new(Module { name: name.to_string(), base, size: hi - lo, bias: base.wrapping_sub(lo), ..Default::default() });

    // Host address of vaddr 0 (below the image when the first segment does not start at 0).
    let mut d = DynView { origin: (base as usize).wrapping_sub(lo as usize), lo, hi, ..Default::default() };
    let (mut dyn_vaddr, mut dyn_size) = (0u32, 0u32);
    for i in 0..eh.e_phnum as usize {
        let p = phdr(&file, &eh, i);
        match p.p_type {
            PT_LOAD => {
                std::ptr::copy_nonoverlapping(file.as_ptr().add(p.p_offset as usize), g2h::<u8>(m.bias.wrapping_add(p.p_vaddr)), p.p_filesz as usize);
                let ph_end = eh.e_phoff as u64 + eh.e_phnum as u64 * std::mem::size_of::<Phdr>() as u64;
                if p.p_offset <= eh.e_phoff && ph_end <= p.p_offset as u64 + p.p_filesz as u64 {
                    m.phdr = m.bias.wrapping_add(p.p_vaddr).wrapping_add(eh.e_phoff - p.p_offset);
                    m.phnum = eh.e_phnum as u32;
                }
            }
            PT_DYNAMIC => {
                dyn_vaddr = p.p_vaddr;
                dyn_size = p.p_memsz;
            }
            PT_ARM_EXIDX => {
                m.exidx = m.bias.wrapping_add(p.p_vaddr);
                m.exidx_count = p.p_memsz / 8;
            }
            PT_TLS => {
                if p.p_memsz != 0 {
                    logw!("{}: uses ELF TLS, which is not supported yet", name);
                }
            }
            _ => {}
        }
    }
    drop(file);
    let parsed = if dyn_size == 0 { Err("no dynamic section".to_string()) } else { parse_dynamic(&mut d, dyn_vaddr, dyn_size) };
    if let Err(e) = parsed {
        page_free(m.base, m.size);
        return Err(format!("{}: {}", name, e));
    }
    m.symbolic = d.symbolic;
    for i in 1..d.nsyms {
        let s = d.sym(i);
        if s.st_shndx == 0 {
            continue;
        }
        let bind = s.st_info >> 4;
        if bind != STB_GLOBAL && bind != STB_WEAK {
            continue;
        }
        m.exports.entry(d.str(s.st_name)).or_insert(m.bias.wrapping_add(s.st_value));
    }
    for &off in &d.needed_off {
        m.needed.push(d.str(off));
    }
    m.init_func = if d.init != 0 { m.bias.wrapping_add(d.init) } else { 0 };
    m.init_array = if d.init_array != 0 { m.bias.wrapping_add(d.init_array) } else { 0 };
    m.init_array_count = d.init_arraysz / 4;
    let m: ModRef = Box::leak(m);
    lock(&MODULES).push(m);
    logi!("loaded {} at 0x{:08x}-0x{:08x} ({} exports)", name, m.base, m.base + m.size, m.exports.len());
    crate::frame_cap::frame_cap_scan(m);

    for dep in &m.needed {
        let dn = base_name(dep);
        if loader_is_system_lib(dn) {
            continue;
        }
        if let Err(derr) = load_locked(dn) {
            logw!("{}: dependency {} unavailable ({})", name, dn, derr);
        }
    }

    let mut result: Result<(), String> = Ok(());
    if d.relr != 0 {
        let mut where_ = 0u32;
        for i in 0..(d.relrsz / 4) as usize {
            let e = std::ptr::read_unaligned((d.relr + i * 4) as *const u32);
            let bump = |off: u32| {
                let a = m.bias.wrapping_add(off);
                wr(a, rd::<u32>(a).wrapping_add(m.bias));
            };
            if e & 1 == 0 {
                where_ = e;
                bump(where_);
                where_ = where_.wrapping_add(4);
            } else {
                let mut off = where_;
                let mut bits = e >> 1;
                while bits != 0 {
                    if bits & 1 != 0 {
                        bump(off);
                    }
                    bits >>= 1;
                    off = off.wrapping_add(4);
                }
                where_ = where_.wrapping_add(31 * 4);
            }
        }
    }
    if d.android_rel != 0 {
        match unpack_android_rel(std::slice::from_raw_parts(d.android_rel as *const u8, d.android_relsz as usize)) {
            None => result = Err("bad packed relocations".into()),
            Some(unpacked) => result = apply_rel(m, &d, unpacked.as_ptr(), unpacked.len() as u32),
        }
    }
    if result.is_ok() && d.rel != 0 {
        result = apply_rel(m, &d, d.rel as *const Rel, d.relsz / 8);
    }
    if result.is_ok() && d.jmprel != 0 {
        result = apply_rel(m, &d, d.jmprel as *const Rel, d.jmprelsz / 8);
    }
    if let Err(e) = result {
        lock(&MODULES).retain(|x| !std::ptr::eq(*x, m));
        page_free(m.base, m.size);
        return Err(format!("{}: {}", name, e));
    }
    mem_bump_code_epoch();
    Ok(m)
}

fn run_constructors(m: ModRef) {
    if m.constructed.swap(true, Ordering::SeqCst) {
        return;
    }
    for dep in &m.needed {
        if let Some(dm) = loader_find(base_name(dep)) {
            run_constructors(dm);
        }
    }
    let c = Cpu::current();
    if m.init_func != 0 {
        c.call(m.init_func, &[]);
    }
    for i in 0..m.init_array_count {
        let f: u32 = unsafe { rd(m.init_array + i * 4) };
        if f != 0 && f != 0xFFFF_FFFF {
            c.call(f, &[]);
        }
    }
}

pub fn loader_add_provider(p: LibProvider) {
    let _g = LOAD_MU.lock();
    lock(&PROVIDERS).push(Box::leak(Box::new(p)));
}

/// True for libraries whose functions are provided by the bridge (libc.so, libm.so, ...).
pub fn loader_is_system_lib(name: &str) -> bool {
    SYSTEM_LIBS.contains(&name)
}

/// Loads a library (and its dependencies), relocates it and runs its constructors.
pub fn loader_load(path: &str) -> Result<ModRef, String> {
    thunks_init();
    let _g = LOAD_MU.lock();
    match unsafe { load_locked(base_name(path)) } {
        Ok(m) => {
            run_constructors(m);
            Ok(m)
        }
        Err(e) => {
            loge!("load failed: {}", e);
            Err(e)
        }
    }
}

pub fn loader_find(name: &str) -> Option<ModRef> {
    let _g = LOAD_MU.lock();
    modules_snapshot().into_iter().find(|m| m.name == name)
}

pub fn loader_module_for(addr: u32) -> Option<ModRef> {
    let _g = LOAD_MU.lock();
    modules_snapshot().into_iter().find(|m| addr >= m.base && (addr as u64) < m.base as u64 + m.size as u64)
}

pub fn loader_modules() -> Vec<ModRef> {
    let _g = LOAD_MU.lock();
    modules_snapshot()
}

/// Guest-defined symbol lookup across all loaded modules.
pub fn loader_lookup(sym: &str) -> u32 {
    let _g = LOAD_MU.lock();
    modules_snapshot().into_iter().map(|m| m.find(sym)).find(|&a| a != 0).unwrap_or(0)
}

/// "libfoo.so+0x1234 (symbol)".
pub fn describe_address(addr: u32) -> String {
    if let Some(t) = trap_name_at(addr) {
        return format!("0x{:08x} <host:{}>", addr, t);
    }
    // Also used from the crash handler: never wait for a lock another thread may hold forever.
    let Some(_g) = LOAD_MU.try_lock() else { return format!("0x{:08x}", addr) };
    let Ok(mods) = MODULES.try_lock() else { return format!("0x{:08x}", addr) };
    let Some(m) = mods.iter().find(|m| addr >= m.base && (addr as u64) < m.base as u64 + m.size as u64).copied() else {
        return format!("0x{:08x} <unknown>", addr);
    };
    drop(mods);
    let mut best: Option<(&str, u32)> = None;
    for (n, &a) in &m.exports {
        let a = a & !1;
        if a <= addr && a > best.map(|b| b.1).unwrap_or(0) {
            best = Some((n, a));
        }
    }
    match best {
        None => format!("0x{:08x} {}+0x{:x}", addr, m.name, addr.wrapping_sub(m.bias)),
        Some((n, a)) => format!("0x{:08x} {}+0x{:x} ({}+0x{:x})", addr, m.name, addr.wrapping_sub(m.bias), n, addr - a),
    }
}

#[derive(Default)]
pub struct ElfInfo {
    pub valid: bool,
    pub arm32: bool,
    pub error: String,
    pub machine: String,
    pub needed: Vec<String>,
    pub imports: Vec<String>, // undefined symbols
    pub exports: Vec<String>,
}

pub fn elf_inspect(data: &[u8]) -> ElfInfo {
    let mut info = ElfInfo::default();
    if let Err(e) = check_header(data, Some(&mut info.machine)) {
        info.error = e;
        info.valid = data.len() >= 4 && &data[..4] == b"\x7fELF";
        return info;
    }
    info.valid = true;
    info.arm32 = true;
    unsafe {
        let eh = ehdr(data);
        let (mut hi, mut dyn_vaddr, mut dyn_size) = (0u32, 0u32, 0u32);
        for i in 0..eh.e_phnum as usize {
            let p = phdr(data, &eh, i);
            if p.p_type == PT_LOAD {
                if p.p_offset as u64 + p.p_filesz as u64 > data.len() as u64 {
                    info.error = "corrupt segment".into();
                    return info;
                }
                hi = hi.max(p.p_vaddr.wrapping_add(p.p_memsz));
            } else if p.p_type == PT_DYNAMIC {
                dyn_vaddr = p.p_vaddr;
                dyn_size = p.p_memsz;
            }
        }
        if hi == 0 || hi > (512 << 20) {
            info.error = "unreasonable image size".into();
            return info;
        }
        let mut image = vec![0u8; hi as usize];
        for i in 0..eh.e_phnum as usize {
            let p = phdr(data, &eh, i);
            if p.p_type == PT_LOAD && p.p_filesz <= p.p_memsz {
                image[p.p_vaddr as usize..p.p_vaddr as usize + p.p_filesz as usize].copy_from_slice(&data[p.p_offset as usize..(p.p_offset + p.p_filesz) as usize]);
            }
        }
        let mut d = DynView { origin: image.as_ptr() as usize, lo: 0, hi, ..Default::default() };
        let parsed = if dyn_size == 0 { Err("no dynamic section".to_string()) } else { parse_dynamic(&mut d, dyn_vaddr, dyn_size) };
        if let Err(e) = parsed {
            info.error = e;
            return info;
        }
        for &off in &d.needed_off {
            info.needed.push(d.str(off));
        }
        for i in 1..d.nsyms {
            let s = d.sym(i);
            let n = d.str(s.st_name);
            if n.is_empty() {
                continue;
            }
            let bind = s.st_info >> 4;
            if s.st_shndx == 0 {
                if bind != STB_WEAK {
                    info.imports.push(n);
                }
            } else if bind == STB_GLOBAL || bind == STB_WEAK {
                info.exports.push(n);
            }
        }
    }
    info
}
