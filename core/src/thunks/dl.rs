//! The dynamic linker API (dlopen, dlsym, dladdr, ...), compatible with Android before 5.0.
//!
//! Back then a dlopen() handle was a pointer to the linker's own record of the library (struct soinfo).
//! Some games ship a copy of that linker (Glu's Unity games load their real libraries with it) and look
//! symbols up directly in such records: in the ELF hash table they point to. So every handle given out
//! here is such a record in guest memory, with the old layout and a hash table of the library's symbols:
//!
//!   0x00 name[128]   0x8c base   0x90 size   0xa4 next   0xa8 flags   0xac strtab   0xb0 symtab
//!   0xb4 nbucket     0xb8 nchain 0xbc bucket 0xc0 chain
//!
//! Libraries that such a built-in linker maps itself may need functions the bridge does not have; they
//! get placeholders (see add_imports_of_foreign_images) so that linking succeeds and only a real call
//! reports the missing function.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::cell::RefCell;
use std::collections::HashMap;
use std::os::raw::c_char;
use std::sync::atomic::{AtomicU32, Ordering};

use crate::elf::*;
use crate::mem::*;
use crate::thunk::{reg, reg_raw, thunk_exists, thunk_missing, thunk_resolve, thunk_resolve_all, ArgReader};
use crate::thunks::libc::intern_str;
use crate::util::{base_name, cstr, Racy, RecMutex};

// ------------------------------------------------------------------ dlerror
thread_local! {
    static DLERROR: RefCell<Option<String>> = const { RefCell::new(None) };
}
static DLERROR_BUF: AtomicU32 = AtomicU32::new(0);

fn set_dlerror(e: String) {
    DLERROR.with(|d| *d.borrow_mut() = Some(e));
}

// ------------------------------------------------------------------ ELF symbol tables in guest memory
type SymbolList = Vec<(String, u32)>; // name, absolute guest address

#[derive(Clone, Copy, Default)]
struct SymbolTable {
    strtab: u32,
    symtab: u32,
    nbucket: u32,
    nchain: u32,
    bucket: u32,
    chain: u32,
}

fn elf_hash(name: &[u8]) -> u32 {
    let mut h = 0u32;
    for &b in name {
        h = (h << 4).wrapping_add(b as u32);
        let g = h & 0xf000_0000;
        h ^= g;
        h ^= g >> 24;
    }
    h
}

// Builds strtab, symtab (Elf32_Sym: name, value, size, info, other, shndx) and the ELF hash table.
unsafe fn build_table(syms: &SymbolList) -> SymbolTable {
    let n = syms.len() as u32;
    let str_size: usize = 1 + syms.iter().map(|s| s.0.len() + 1).sum::<usize>();
    let mut t = SymbolTable { strtab: g_calloc(1, str_size as u32), symtab: g_calloc(n + 1, 16), nbucket: n | 1, nchain: n + 1, ..Default::default() };
    t.bucket = g_calloc(t.nbucket, 4);
    t.chain = g_calloc(t.nchain, 4);
    let mut str_off = 1u32;
    for (i, (name, value)) in syms.iter().enumerate() {
        let i = i as u32;
        std::ptr::copy_nonoverlapping(name.as_ptr(), g2h::<u8>(t.strtab + str_off), name.len());
        let sym = t.symtab + (i + 1) * 16;
        wr::<u32>(sym, str_off);
        wr::<u32>(sym + 4, *value); // the record's base is 0: values are addresses
        wr::<u8>(sym + 12, (1 << 4) | 2); // STB_GLOBAL, STT_FUNC
        wr::<u16>(sym + 14, 1); // defined
        let b = elf_hash(name.as_bytes()) % t.nbucket;
        wr::<u32>(t.chain + (i + 1) * 4, rd::<u32>(t.bucket + b * 4));
        wr::<u32>(t.bucket + b * 4, i + 1);
        str_off += name.len() as u32 + 1;
    }
    t
}

// ------------------------------------------------------------------ old-style handles (soinfo)
#[derive(Default)]
struct State {
    handles: HashMap<u32, Option<ModRef>>,  // soinfo address -> what it stands for (None: a library implemented by the bridge)
    system_soinfos: HashMap<String, u32>,   // library name -> soinfo
    module_soinfos: HashMap<usize, u32>,    // Module address -> soinfo
    system_table: SymbolTable,
    placeholders: HashMap<String, u32>,     // missing symbol -> stub or zeroed data
    scanned_images: Vec<u32>,
}
static MU: RecMutex = RecMutex::new();
// Only touched while MU is held.
static STATE: Racy<Option<State>> = Racy::new(None);
unsafe fn state() -> &'static mut State {
    STATE.get().get_or_insert_with(State::default)
}

unsafe fn write_table(si: u32, t: &SymbolTable) {
    wr::<u32>(si + 0xac, t.strtab);
    wr::<u32>(si + 0xb0, t.symtab);
    wr::<u32>(si + 0xb4, t.nbucket);
    wr::<u32>(si + 0xb8, t.nchain);
    wr::<u32>(si + 0xbc, t.bucket);
    wr::<u32>(si + 0xc0, t.chain);
}

unsafe fn new_soinfo(name: &str, t: &SymbolTable, m: Option<ModRef>) -> u32 {
    let si = g_calloc(1, 1024);
    let n = name.len().min(127);
    std::ptr::copy_nonoverlapping(name.as_ptr(), g2h::<u8>(si), n);
    wr::<u32>(si + 0xa8, 1); // FLAG_LINKED
    write_table(si, t);
    state().handles.insert(si, m);
    si
}

// ------------------------------------------------------------------ placeholders for foreign libraries
fn provided(name: &str) -> bool {
    thunk_exists(name) || loader_lookup(name) != 0
}

// Reads the undefined symbols of an ELF image that is mapped in guest memory (header at `a`) and gives
// every one the bridge lacks a placeholder. Returns how many were added.
unsafe fn add_placeholders_for_image(a: u32) -> u32 {
    let page = mem_page_size();
    let (phoff, phnum) = (rd::<u32>(a + 28), rd::<u16>(a + 44) as u32);
    if phnum > 32 || phoff > page - 32 * phnum {
        return 0;
    }
    let (mut bias, mut dynv, mut have_load) = (0u32, 0u32, false);
    for i in 0..phnum {
        let ph = a + phoff + i * 32;
        let (ty, vaddr) = (rd::<u32>(ph), rd::<u32>(ph + 8));
        if ty == 1 && !have_load {
            // the first PT_LOAD maps the header
            bias = a.wrapping_sub(vaddr & !(page - 1));
            have_load = true;
        }
        if ty == 2 {
            dynv = vaddr; // PT_DYNAMIC
        }
    }
    if !have_load || dynv == 0 {
        return 0;
    }
    // A linker may or may not have relocated the dynamic section in place.
    let addr = |v: u32| if v >= bias && mem_is_mapped(v, 4) && v - bias < 0x1000_0000 { v } else { bias.wrapping_add(v) };
    let d = bias.wrapping_add(dynv);
    let (mut symtab, mut strtab, mut hash) = (0u32, 0u32, 0u32);
    for i in 0..256u32 {
        if !mem_is_mapped(d + i * 8, 8) {
            break;
        }
        let (tag, val) = (rd::<u32>(d + i * 8), rd::<u32>(d + i * 8 + 4));
        match tag {
            0 => break,
            4 => hash = addr(val),   // DT_HASH
            5 => strtab = addr(val), // DT_STRTAB
            6 => symtab = addr(val), // DT_SYMTAB
            _ => {}
        }
    }
    if symtab == 0 || strtab == 0 || hash == 0 || !mem_is_mapped(hash, 8) {
        return 0;
    }
    let nsyms = rd::<u32>(hash + 4);
    let mut added = 0;
    for i in 1..nsyms {
        let sym = symtab.wrapping_add(i.wrapping_mul(16));
        if !mem_is_mapped(sym, 16) {
            break;
        }
        let name_off = rd::<u32>(sym);
        let info = rd::<u8>(sym + 12);
        if rd::<u16>(sym + 14) != 0 || name_off == 0 || (info >> 4) == 2 {
            continue; // defined, unnamed, weak
        }
        if !mem_is_mapped(strtab.wrapping_add(name_off), 1) {
            continue;
        }
        let name = cstr(g2h::<c_char>(strtab.wrapping_add(name_off)));
        if name.is_empty() || provided(&name) || state().placeholders.contains_key(&name) {
            continue;
        }
        // Data gets a zeroed block, functions a stub that reports the call and returns 0.
        let v = if (info & 0xf) == 1 { g_calloc(1, 256) } else { thunk_missing(&name) };
        state().placeholders.insert(name, v);
        added += 1;
    }
    added
}

// Looks for ELF images in guest memory that our loader did not load (mapped by a linker inside the
// app) and adds placeholders for what they import. Returns true if any were added.
unsafe fn add_imports_of_foreign_images() -> bool {
    let mut added = false;
    for (start, size) in mem_used_regions() {
        let mut a = start as u64;
        while a + 64 <= start as u64 + size as u64 {
            let at = a as u32;
            a += mem_page_size() as u64;
            if rd::<[u8; 5]>(at) != [0x7f, b'E', b'L', b'F', 1] {
                continue;
            }
            if rd::<u16>(at + 16) != 3 || rd::<u16>(at + 18) != 40 {
                continue; // ET_DYN, EM_ARM
            }
            if loader_module_for(at).is_some() || state().scanned_images.contains(&at) {
                continue; // loaded by us, or seen before
            }
            state().scanned_images.push(at);
            let n = add_placeholders_for_image(at);
            if n != 0 {
                let list: Vec<&str> = state().placeholders.keys().map(|s| s.as_str()).collect();
                logw!(
                    "library loaded by the app's own linker at 0x{:08x} needs {} functions the bridge does not have (placeholders so far: {})",
                    at,
                    n,
                    list.join(" ")
                );
                added = true;
            }
        }
    }
    added
}

// ------------------------------------------------------------------ handles for libraries

// The handle of a library implemented by the bridge. All of them share one table: every bridge symbol
// and the placeholders. It is rebuilt when a foreign library brought new placeholders.
fn system_handle(name: &str) -> u32 {
    let _g = MU.lock();
    unsafe {
        if add_imports_of_foreign_images() || state().system_table.symtab == 0 {
            let mut syms = thunk_resolve_all();
            syms.extend(state().placeholders.iter().map(|(k, &v)| (k.clone(), v)));
            let t = build_table(&syms);
            state().system_table = t;
            for &si in state().system_soinfos.values() {
                write_table(si, &t);
            }
            logi!("dlopen: symbol table of the bridge for old-style handles: {} symbols", syms.len());
        }
        if let Some(&si) = state().system_soinfos.get(name) {
            return si;
        }
        let t = state().system_table;
        let si = new_soinfo(name, &t, None);
        state().system_soinfos.insert(name.to_string(), si);
        si
    }
}

// The handle of a library our loader loaded: its exports.
fn module_handle(m: ModRef) -> u32 {
    let _g = MU.lock();
    unsafe {
        let key = m as *const Module as usize;
        if let Some(&si) = state().module_soinfos.get(&key) {
            return si;
        }
        let syms: SymbolList = m.exports.iter().map(|(k, &v)| (k.clone(), v)).collect();
        let t = build_table(&syms);
        let si = new_soinfo(&m.name, &t, Some(m));
        wr::<u32>(si + 0x8c, 0); // symbol values are absolute (base 0)
        wr::<u32>(si + 0x90, m.size);
        state().module_soinfos.insert(key, si);
        si
    }
}

// What a handle stands for: (found, module); found = false for something that is not one of our handles.
fn lookup_handle(h: u32) -> (bool, Option<ModRef>) {
    let _g = MU.lock();
    if let Some(&m) = unsafe { state() }.handles.get(&h) {
        return (true, m);
    }
    // handles given out as the load address (older behaviour)
    match loader_modules().into_iter().find(|m| m.base == h) {
        Some(m) => (true, Some(m)),
        None => (false, None),
    }
}

fn placeholder(name: &str) -> u32 {
    let _g = MU.lock();
    unsafe { state() }.placeholders.get(name).copied().unwrap_or(0)
}

pub fn register_dl_thunks() {
    reg("dlopen", |name: *const c_char, _flags: i32| -> u32 {
        if name.is_null() {
            return system_handle("");
        }
        let full = unsafe { cstr(name) };
        let n = base_name(&full);
        if loader_is_system_lib(n) {
            return system_handle(n);
        }
        match loader_load(n) {
            Ok(m) => module_handle(m),
            Err(e) => {
                set_dlerror(e);
                0
            }
        }
    });
    reg("dlclose", |_h: u32| -> i32 { 0 });
    reg("dlsym", |handle: u32, name: *const c_char| -> u32 {
        let name = unsafe { cstr(name) };
        let (found, module) = lookup_handle(handle);
        let mut a = module.map(|m| m.find(&name)).unwrap_or(0);
        if a == 0 && module.is_none() {
            // a bridge library, RTLD_DEFAULT/RTLD_NEXT or an unknown handle
            if !found {
                a = loader_lookup(&name);
            }
            if a == 0 {
                a = thunk_resolve(&name);
            }
            if a == 0 {
                a = placeholder(&name);
            }
        }
        if a == 0 {
            set_dlerror(format!("undefined symbol: {}", name));
        }
        a
    });
    reg("dlerror", || -> u32 {
        let Some(e) = DLERROR.with(|d| d.borrow_mut().take()) else { return 0 };
        let mut buf = DLERROR_BUF.load(Ordering::Relaxed);
        if buf == 0 {
            buf = g_calloc(1, 512);
            DLERROR_BUF.store(buf, Ordering::Relaxed);
        }
        let n = e.len().min(511);
        unsafe {
            std::ptr::copy_nonoverlapping(e.as_ptr(), g2h::<u8>(buf), n);
            wr::<u8>(buf + n as u32, 0);
        }
        buf
    });
    reg("dladdr", |addr: u32, info: *mut u32| -> i32 {
        let Some(m) = loader_module_for(addr) else { return 0 };
        unsafe {
            *info = intern_str(&m.name);
            *info.add(1) = m.base;
            *info.add(2) = 0;
            *info.add(3) = 0;
        }
        1
    });

    // Exception unwinding tables of loaded libraries.
    for n in ["__gnu_Unwind_Find_exidx", "dl_unwind_find_exidx"] {
        reg(n, |pc: u32, count: *mut i32| -> u32 {
            let m = loader_module_for(pc).filter(|m| m.exidx != 0);
            unsafe {
                if !count.is_null() {
                    *count = m.map(|m| m.exidx_count as i32).unwrap_or(0);
                }
            }
            m.map(|m| m.exidx).unwrap_or(0)
        });
    }
    reg_raw("dl_iterate_phdr", |c| {
        let mut a = ArgReader::new(c);
        let (cb, data) = (a.next32(), a.next32());
        let info = g_calloc(1, 32);
        let mut ret = 0u32;
        for m in loader_modules() {
            if m.phdr == 0 {
                continue;
            }
            unsafe {
                wr::<u32>(info, m.bias);
                wr::<u32>(info + 4, intern_str(&m.name));
                wr::<u32>(info + 8, m.phdr);
                wr::<u32>(info + 12, m.phnum); // Elf32_Half followed by padding
            }
            ret = c.call(cb, &[info, 16, data]) as u32;
            if ret != 0 {
                break;
            }
        }
        g_free(info);
        c.set_ret32(ret);
    });
}
