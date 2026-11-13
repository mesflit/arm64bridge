//! /proc files as Java code of a bridged app sees them.
//!
//! Some apps check the CPU from Java, not only from native code. The Java player of Unity 3 reads
//! /proc/cpuinfo with a FileReader and needs "vfp" on the Features line and a CPU architecture of 6
//! or more; otherwise it loads its "compatible mode" and turns audio off (Samurai vs Zombies:
//! "Application was unable to load optimized libraries"). An arm64 kernel lists "fp asimd ...".
//!
//! Java opens files through the open() of libjavacore (libcore.io.Linux.open on Android) or of
//! libopenjdk / libjava (FileInputStream on a desktop JVM). Their import of open() is redirected here
//! (the GOT entry is rewritten), and /proc files that guest_proc_open() knows get the guest view.
//! The patched app loads the runtime from a content provider (BridgeStarter), so this is in place
//! before the app's own classes run.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Once;

use crate::proc::guest_proc_open;

// open() is variadic; its optional third argument (the mode) is passed like a fixed one on the
// supported hosts (x86-64 and AArch64 Linux), so the hooks take it as an ordinary parameter. It is
// only meaningful when the flags ask for a file to be created, exactly as in open() itself.
type OpenFn = unsafe extern "C" fn(*const c_char, c_int, c_int) -> c_int;
type Open2Fn = unsafe extern "C" fn(*const c_char, c_int) -> c_int;

static G_OPEN: AtomicUsize = AtomicUsize::new(0);
static G_OPEN64: AtomicUsize = AtomicUsize::new(0);
static G_OPEN_2: AtomicUsize = AtomicUsize::new(0);
static G_OPEN64_2: AtomicUsize = AtomicUsize::new(0);

unsafe fn call3(slot: &AtomicUsize, path: *const c_char, flags: c_int, mode: c_int) -> c_int {
    let f: OpenFn = std::mem::transmute(slot.load(Ordering::Relaxed));
    let needs_mode = (flags & libc::O_CREAT) != 0 || (flags & libc::O_TMPFILE) == libc::O_TMPFILE;
    f(path, flags, if needs_mode { mode } else { 0 })
}
unsafe fn call2(slot: &AtomicUsize, path: *const c_char, flags: c_int) -> c_int {
    let f: Open2Fn = std::mem::transmute(slot.load(Ordering::Relaxed));
    f(path, flags)
}

unsafe extern "C" fn hook_open(path: *const c_char, flags: c_int, mode: c_int) -> c_int {
    let v = guest_proc_open(path);
    if v != -2 { v } else { call3(&G_OPEN, path, flags, mode) }
}
unsafe extern "C" fn hook_open64(path: *const c_char, flags: c_int, mode: c_int) -> c_int {
    let v = guest_proc_open(path);
    if v != -2 { v } else { call3(&G_OPEN64, path, flags, mode) }
}
unsafe extern "C" fn hook_open_2(path: *const c_char, flags: c_int) -> c_int {
    let v = guest_proc_open(path);
    if v != -2 { v } else { call2(&G_OPEN_2, path, flags) }
}
unsafe extern "C" fn hook_open64_2(path: *const c_char, flags: c_int) -> c_int {
    let v = guest_proc_open(path);
    if v != -2 { v } else { call2(&G_OPEN64_2, path, flags) }
}

fn hooks() -> [(&'static [u8], usize); 4] {
    [
        (b"open", hook_open as *const () as usize),
        (b"open64", hook_open64 as *const () as usize),
        (b"__open_2", hook_open_2 as *const () as usize),
        (b"__open64_2", hook_open64_2 as *const () as usize),
    ]
}

const JAVA_LIBS: [&std::ffi::CStr; 3] = [c"libjavacore.so", c"libopenjdk.so", c"libjava.so"];

unsafe fn is_java_lib(path: *const c_char) -> bool {
    if path.is_null() {
        return false;
    }
    let full = crate::util::cbytes(path);
    let base = full.rsplit(|&b| b == b'/').next().unwrap_or(full);
    JAVA_LIBS.iter().any(|l| l.to_bytes() == base)
}

#[repr(C)]
struct Dyn64 {
    d_tag: i64,
    d_val: u64,
}
#[repr(C)]
struct Sym64 {
    st_name: u32,
    st_info: u8,
    st_other: u8,
    st_shndx: u16,
    st_value: u64,
    st_size: u64,
}
#[repr(C)]
struct Rela64 {
    r_offset: u64,
    r_info: u64,
    r_addend: i64,
}
const PT_DYNAMIC: u32 = 2;
const PT_GNU_RELRO: u32 = 0x6474_e552;
const DT_PLTRELSZ: i64 = 2;
const DT_STRTAB: i64 = 5;
const DT_SYMTAB: i64 = 6;
const DT_RELA: i64 = 7;
const DT_RELASZ: i64 = 8;
const DT_JMPREL: i64 = 23;

// Rewrites the GOT entries of one library for the hooked functions.
unsafe fn patch_library(info: *const libc::dl_phdr_info) -> i32 {
    let base = (*info).dlpi_addr as usize;
    let mut dynp: *const Dyn64 = std::ptr::null();
    let (mut relro_start, mut relro_end) = (0usize, 0usize);
    for i in 0..(*info).dlpi_phnum as usize {
        let ph = &*(*info).dlpi_phdr.add(i);
        if ph.p_type == PT_DYNAMIC {
            dynp = (base + ph.p_vaddr as usize) as *const Dyn64;
        }
        if ph.p_type == PT_GNU_RELRO {
            relro_start = base + ph.p_vaddr as usize;
            relro_end = relro_start + ph.p_memsz as usize;
        }
    }
    if dynp.is_null() {
        return 0;
    }
    // glibc stores absolute addresses in the dynamic section, bionic leaves them relative.
    let ptr = |v: u64| if (v as usize) < base { v as usize + base } else { v as usize };
    let (mut strtab, mut symtab, mut jmprel, mut rela) = (0usize, 0usize, 0usize, 0usize);
    let (mut jmprel_size, mut rela_size) = (0usize, 0usize);
    let mut d = dynp;
    while (*d).d_tag != 0 {
        match (*d).d_tag {
            DT_STRTAB => strtab = ptr((*d).d_val),
            DT_SYMTAB => symtab = ptr((*d).d_val),
            DT_JMPREL => jmprel = ptr((*d).d_val),
            DT_PLTRELSZ => jmprel_size = (*d).d_val as usize,
            DT_RELA => rela = ptr((*d).d_val),
            DT_RELASZ => rela_size = (*d).d_val as usize,
            _ => {}
        }
        d = d.add(1);
    }
    if strtab == 0 || symtab == 0 {
        return 0;
    }
    let page = libc::sysconf(libc::_SC_PAGESIZE) as usize;
    let mut patched = 0;
    for (start, size) in [(jmprel, jmprel_size), (rela, rela_size)] {
        if start == 0 {
            continue;
        }
        let r = start as *const Rela64;
        for i in 0..size / std::mem::size_of::<Rela64>() {
            let rel = &*r.add(i);
            let sym = (rel.r_info >> 32) as usize;
            if sym == 0 {
                continue;
            }
            let name = crate::util::cbytes((strtab + (*(symtab as *const Sym64).add(sym)).st_name as usize) as *const c_char);
            for (hook_name, hook_fn) in hooks() {
                if name != hook_name {
                    continue;
                }
                let slot = base + rel.r_offset as usize;
                let page_start = (slot & !(page - 1)) as *mut c_void;
                let relro = slot >= relro_start && slot < relro_end;
                if relro && libc::mprotect(page_start, page, libc::PROT_READ | libc::PROT_WRITE) != 0 {
                    break;
                }
                *(slot as *mut usize) = hook_fn;
                if relro {
                    libc::mprotect(page_start, page, libc::PROT_READ);
                }
                patched += 1;
            }
        }
    }
    patched
}

unsafe extern "C" fn phdr_callback(info: *mut libc::dl_phdr_info, _size: usize, data: *mut c_void) -> c_int {
    if is_java_lib((*info).dlpi_name) {
        *(data as *mut i32) += patch_library(info);
    }
    0
}

/// Lets Java code see the guest's view of /proc files too.
pub fn java_proc_hook_install() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let sym = |name: &std::ffi::CStr| libc::dlsym(libc::RTLD_DEFAULT, name.as_ptr()) as usize;
        let (open, open_2) = (sym(c"open"), sym(c"__open_2"));
        let (open64, open64_2) = (sym(c"open64"), sym(c"__open64_2"));
        G_OPEN.store(open, Ordering::SeqCst);
        G_OPEN64.store(if open64 != 0 { open64 } else { open }, Ordering::SeqCst);
        G_OPEN_2.store(open_2, Ordering::SeqCst);
        G_OPEN64_2.store(if open64_2 != 0 { open64_2 } else { open_2 }, Ordering::SeqCst);
        if open == 0 {
            logw!("Java /proc view: open() not found");
            return;
        }
        // Make sure the Java libraries are loaded, then patch them.
        for l in JAVA_LIBS {
            libc::dlopen(l.as_ptr(), libc::RTLD_NOW | libc::RTLD_NOLOAD);
        }
        let mut total: i32 = 0;
        libc::dl_iterate_phdr(Some(phdr_callback), &mut total as *mut i32 as *mut c_void);
        logi!("Java /proc view: {} imports redirected (Java code sees the /proc/cpuinfo of a 32-bit ARM CPU)", total);
    });
}
