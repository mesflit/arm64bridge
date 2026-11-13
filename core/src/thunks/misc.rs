//! ARM EABI helpers, setjmp/longjmp, raw system calls and data symbols exported by bionic.
//! (The dynamic linker API is in dl.rs.)
// SPDX-License-Identifier: GPL-2.0-or-later
use crate::cpu::{BlockingCall, Cpu};
use crate::guest_types::*;
use crate::mem::*;
use crate::thunk::{reg, reg_raw, thunk_register_data};
use crate::util::errno;

// jmp_buf layout used by the bridge: [0..7] r4-r11, [8] sp, [9] lr, [10] magic, [12..27] d8-d15
const JMP_MAGIC: u32 = 0xA64B_5E7A;

fn do_setjmp(c: &mut Cpu) {
    unsafe {
        let buf = c.r(0);
        for i in 0..8 {
            wr::<u32>(buf + i * 4, c.r(4 + i as usize));
        }
        wr::<u32>(buf + 32, c.r(13));
        wr::<u32>(buf + 36, c.r(14));
        wr::<u32>(buf + 40, JMP_MAGIC);
        let ext = c.ext_regs();
        if !ext.is_null() {
            std::ptr::copy_nonoverlapping(ext.add(16) as *const u8, g2h::<u8>(buf + 48), 64);
        }
    }
    c.set_ret32(0);
}

fn do_longjmp(c: &mut Cpu) {
    unsafe {
        let buf = c.r(0);
        let val = c.r(1);
        if rd::<u32>(buf + 40) != JMP_MAGIC {
            fatal!("longjmp with a jmp_buf that was not filled by setjmp");
        }
        for i in 0..8 {
            c.set_r(4 + i as usize, rd::<u32>(buf + i * 4));
        }
        c.set_r(13, rd::<u32>(buf + 32));
        c.set_r(14, rd::<u32>(buf + 36));
        c.set_r(15, rd::<u32>(buf + 36));
        let ext = c.ext_regs();
        if !ext.is_null() {
            std::ptr::copy_nonoverlapping(g2h::<u8>(buf + 48), ext.add(16) as *mut u8, 64);
        }
        c.set_r(0, if val != 0 { val } else { 1 });
        c.pc_override = true;
    }
}

fn r01(c: &Cpu) -> u64 {
    c.r(0) as u64 | ((c.r(1) as u64) << 32)
}
fn r23(c: &Cpu) -> u64 {
    c.r(2) as u64 | ((c.r(3) as u64) << 32)
}

/// Raw system call issued by guest code (ARM EABI numbers).
pub fn guest_syscall_args(c: &mut Cpu, nr: u32, a: &[u32; 6]) -> u32 {
    let wrap = |r: i64| -> u32 { if r < 0 { (-errno()) as u32 } else { r as u32 } };
    unsafe {
        match nr {
            // exit, exit_group
            1 | 248 => {
                logw!("guest exit({}) via syscall", a[0] as i32);
                libc::_exit(a[0] as i32);
            }
            3 => wrap(libc::read(a[0] as i32, g2h(a[1]), a[2] as usize) as i64),
            4 => wrap(libc::write(a[0] as i32, g2h(a[1]), a[2] as usize) as i64),
            6 => wrap(libc::close(a[0] as i32) as i64),
            20 => libc::getpid() as u32,
            158 => {
                libc::sched_yield();
                0
            }
            224 => crate::util::gettid() as u32,
            78 => {
                // gettimeofday
                let mut tv: libc::timeval = std::mem::zeroed();
                libc::gettimeofday(&mut tv, std::ptr::null_mut());
                if a[0] != 0 {
                    wr(a[0], GTimeval { tv_sec: tv.tv_sec as i32, tv_usec: tv.tv_usec as i32 });
                }
                0
            }
            263 => {
                // clock_gettime
                let mut ts: libc::timespec = std::mem::zeroed();
                if libc::clock_gettime(a[0] as libc::clockid_t, &mut ts) != 0 {
                    return (-errno()) as u32;
                }
                if a[1] != 0 {
                    wr(a[1], ts_to_guest(&ts));
                }
                0
            }
            240 => {
                // futex(uaddr, op, val, timeout|val2, uaddr2, val3)
                let cmd = a[1] & 0x7f;
                let ts;
                let mut a3 = a[3] as usize;
                if (cmd == 0 /* FUTEX_WAIT */ || cmd == 9/* FUTEX_WAIT_BITSET */) && a[3] != 0 {
                    ts = ts_from_guest(&rd::<GTimespec>(a[3]));
                    a3 = &ts as *const libc::timespec as usize;
                }
                let _blocking = BlockingCall::new();
                wrap(libc::syscall(libc::SYS_futex, a[0] as usize, a[1] as i32, a[2] as i32, a3, a[4] as usize, a[5] as i32) as i64)
            }
            0xf0002 => {
                // ARM cacheflush(start, end, flags)
                mem_invalidate_code(a[0], a[1].saturating_sub(a[0]));
                0
            }
            0xf0005 => {
                // ARM set_tls
                c.tls_block = a[0];
                0
            }
            _ => {
                logw!("unsupported guest system call {}", nr);
                (-libc::ENOSYS) as u32
            }
        }
    }
}

/// Raw "svc 0" system call issued by guest code.
pub fn guest_syscall(c: &mut Cpu, nr: u32) -> u32 {
    let args = [c.r(0), c.r(1), c.r(2), c.r(3), c.r(4), c.r(5)];
    guest_syscall_args(c, nr, &args)
}

fn data_word(name: &str, value: u32) {
    let p = g_calloc(1, 4);
    unsafe { wr::<u32>(p, value) };
    thunk_register_data(name, p);
}

pub fn register_data_symbols() {
    unsafe {
        data_word("__stack_chk_guard", 0x5AFE_0000 | ((libc::getpid() as u32) << 4) | 0xA);
        thunk_register_data("__dso_handle", g_calloc(1, 8));
        data_word("environ", g_calloc(1, 8)); // char** environ -> { NULL }
        data_word("__progname", g_strdup_bytes(b"app_process"));

        // bionic's classic ctype tables: _ctype_ points to 1 + 256 flag bytes.
        let ct = g_calloc(1, 260);
        for ch in 0..128u8 {
            let c = ch as char;
            let mut f = 0u8;
            if c.is_ascii_uppercase() {
                f |= 0x01;
            }
            if c.is_ascii_lowercase() {
                f |= 0x02;
            }
            if c.is_ascii_digit() {
                f |= 0x04;
            }
            if matches!(ch, b' ' | 9..=13) {
                f |= 0x08;
            }
            if c.is_ascii_punctuation() {
                f |= 0x10;
            }
            if c.is_ascii_control() {
                f |= 0x20;
            }
            if c.is_ascii_hexdigit() {
                f |= 0x40;
            }
            if ch == b' ' {
                f |= 0x80;
            }
            wr::<u8>(ct + ch as u32 + 1, f);
        }
        data_word("_ctype_", ct);
        let (lo, up) = (g_calloc(257, 2), g_calloc(257, 2));
        for ch in 0..256u32 {
            let (l, u) = if ch < 128 { ((ch as u8).to_ascii_lowercase() as i16, (ch as u8).to_ascii_uppercase() as i16) } else { (ch as i16, ch as i16) };
            wr::<i16>(lo + (ch + 1) * 2, l);
            wr::<i16>(up + (ch + 1) * 2, u);
        }
        wr::<i16>(lo, -1);
        wr::<i16>(up, -1);
        data_word("_tolower_tab_", lo);
        data_word("_toupper_tab_", up);
    }
}

pub fn register_misc_thunks() {
    // ---------------------------------------------------------------- setjmp / longjmp
    for n in ["setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp"] {
        reg_raw(n, do_setjmp);
    }
    for n in ["longjmp", "_longjmp", "siglongjmp"] {
        reg_raw(n, do_longjmp);
    }

    // ---------------------------------------------------------------- ARM EABI run-time helpers
    // The thread pointer (TPIDRURO), same value as the kernel's __kuser_get_tls.
    reg_raw("__aeabi_read_tp", |c| c.set_ret32(c.tls_block));
    for n in ["__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8"] {
        reg(n, |d: *mut u8, s: *const u8, len: usize| unsafe { std::ptr::copy_nonoverlapping(s, d, len) });
    }
    for n in ["__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"] {
        reg(n, |d: *mut u8, s: *const u8, len: usize| unsafe { std::ptr::copy(s, d, len) });
    }
    for n in ["__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"] {
        reg(n, |d: *mut u8, len: usize, v: i32| unsafe { std::ptr::write_bytes(d, v as u8, len) });
    }
    for n in ["__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"] {
        reg(n, |d: *mut u8, len: usize| unsafe { std::ptr::write_bytes(d, 0, len) });
    }

    for n in ["__aeabi_idiv", "__divsi3"] {
        reg(n, |x: i32, y: i32| -> i32 { if y == 0 { 0 } else { x.wrapping_div(y) } });
    }
    for n in ["__aeabi_uidiv", "__udivsi3"] {
        reg(n, |x: u32, y: u32| -> u32 { x.checked_div(y).unwrap_or(0) });
    }
    reg("__modsi3", |x: i32, y: i32| -> i32 { if y == 0 { 0 } else { x.wrapping_rem(y) } });
    reg("__umodsi3", |x: u32, y: u32| -> u32 { x.checked_rem(y).unwrap_or(0) });
    reg_raw("__aeabi_idivmod", |c| {
        let (x, y) = (c.r(0) as i32, c.r(1) as i32);
        let (q, r) = if y == 0 { (0, 0) } else { (x.wrapping_div(y), x.wrapping_rem(y)) };
        c.set_r(0, q as u32);
        c.set_r(1, r as u32);
    });
    reg_raw("__aeabi_uidivmod", |c| {
        let (x, y) = (c.r(0), c.r(1));
        c.set_r(0, x.checked_div(y).unwrap_or(0));
        c.set_r(1, x.checked_rem(y).unwrap_or(0));
    });
    reg_raw("__aeabi_ldivmod", |c| {
        let (x, y) = (r01(c) as i64, r23(c) as i64);
        let (q, r) = if y == 0 { (0, 0) } else { (x.wrapping_div(y), x.wrapping_rem(y)) };
        c.set_r(0, q as u32);
        c.set_r(1, (q as u64 >> 32) as u32);
        c.set_r(2, r as u32);
        c.set_r(3, (r as u64 >> 32) as u32);
    });
    reg_raw("__aeabi_uldivmod", |c| {
        let (x, y) = (r01(c), r23(c));
        let (q, r) = (x.checked_div(y).unwrap_or(0), x.checked_rem(y).unwrap_or(0));
        c.set_r(0, q as u32);
        c.set_r(1, (q >> 32) as u32);
        c.set_r(2, r as u32);
        c.set_r(3, (r >> 32) as u32);
    });
    reg("__divdi3", |x: i64, y: i64| -> i64 { if y == 0 { 0 } else { x.wrapping_div(y) } });
    reg("__moddi3", |x: i64, y: i64| -> i64 { if y == 0 { 0 } else { x.wrapping_rem(y) } });
    reg("__udivdi3", |x: u64, y: u64| -> u64 { x.checked_div(y).unwrap_or(0) });
    reg("__umoddi3", |x: u64, y: u64| -> u64 { x.checked_rem(y).unwrap_or(0) });
    reg_raw("__aeabi_lmul", |c| c.set_ret64(r01(c).wrapping_mul(r23(c))));
    reg_raw("__aeabi_llsl", |c| c.set_ret64(if c.r(2) >= 64 { 0 } else { r01(c) << c.r(2) }));
    reg_raw("__aeabi_llsr", |c| c.set_ret64(if c.r(2) >= 64 { 0 } else { r01(c) >> c.r(2) }));
    reg_raw("__aeabi_lasr", |c| c.set_ret64(((r01(c) as i64) >> c.r(2).min(63)) as u64));

    // soft-float helpers (libraries built for plain "armeabi")
    fn d0(c: &Cpu) -> f64 {
        f64::from_bits(r01(c))
    }
    fn d1(c: &Cpu) -> f64 {
        f64::from_bits(r23(c))
    }
    fn f0(c: &Cpu) -> f32 {
        f32::from_bits(c.r(0))
    }
    fn f1(c: &Cpu) -> f32 {
        f32::from_bits(c.r(1))
    }
    macro_rules! dop {
        ($($name:literal => |$x:ident, $y:ident| $e:expr;)*) => {$(
            reg_raw($name, |c| { let ($x, $y) = (d0(c), d1(c)); c.set_ret64(f64::to_bits($e)); });
        )*};
    }
    macro_rules! fop {
        ($($name:literal => |$x:ident, $y:ident| $e:expr;)*) => {$(
            reg_raw($name, |c| { let ($x, $y) = (f0(c), f1(c)); c.set_ret32(f32::to_bits($e)); });
        )*};
    }
    macro_rules! dcmp {
        ($($name:literal => |$x:ident, $y:ident| $e:expr;)*) => {$(
            reg_raw($name, |c| { let ($x, $y) = (d0(c), d1(c)); c.set_ret32(($e) as u32); });
        )*};
    }
    macro_rules! fcmp {
        ($($name:literal => |$x:ident, $y:ident| $e:expr;)*) => {$(
            reg_raw($name, |c| { let ($x, $y) = (f0(c), f1(c)); c.set_ret32(($e) as u32); });
        )*};
    }
    dop! {
        "__aeabi_dadd" => |x, y| x + y; "__aeabi_dsub" => |x, y| x - y; "__aeabi_drsub" => |x, y| y - x;
        "__aeabi_dmul" => |x, y| x * y; "__aeabi_ddiv" => |x, y| x / y;
    }
    fop! {
        "__aeabi_fadd" => |x, y| x + y; "__aeabi_fsub" => |x, y| x - y; "__aeabi_frsub" => |x, y| y - x;
        "__aeabi_fmul" => |x, y| x * y; "__aeabi_fdiv" => |x, y| x / y;
    }
    dcmp! {
        "__aeabi_dcmpeq" => |x, y| x == y; "__aeabi_dcmplt" => |x, y| x < y; "__aeabi_dcmple" => |x, y| x <= y;
        "__aeabi_dcmpge" => |x, y| x >= y; "__aeabi_dcmpgt" => |x, y| x > y; "__aeabi_dcmpun" => |x, y| x.is_nan() || y.is_nan();
    }
    fcmp! {
        "__aeabi_fcmpeq" => |x, y| x == y; "__aeabi_fcmplt" => |x, y| x < y; "__aeabi_fcmple" => |x, y| x <= y;
        "__aeabi_fcmpge" => |x, y| x >= y; "__aeabi_fcmpgt" => |x, y| x > y; "__aeabi_fcmpun" => |x, y| x.is_nan() || y.is_nan();
    }
    reg_raw("__aeabi_f2d", |c| c.set_ret64((f0(c) as f64).to_bits()));
    reg_raw("__aeabi_d2f", |c| c.set_ret32((d0(c) as f32).to_bits()));
    reg_raw("__aeabi_i2d", |c| c.set_ret64((c.r(0) as i32 as f64).to_bits()));
    reg_raw("__aeabi_ui2d", |c| c.set_ret64((c.r(0) as f64).to_bits()));
    reg_raw("__aeabi_i2f", |c| c.set_ret32((c.r(0) as i32 as f32).to_bits()));
    reg_raw("__aeabi_ui2f", |c| c.set_ret32((c.r(0) as f32).to_bits()));
    reg_raw("__aeabi_l2d", |c| c.set_ret64((r01(c) as i64 as f64).to_bits()));
    reg_raw("__aeabi_ul2d", |c| c.set_ret64((r01(c) as f64).to_bits()));
    reg_raw("__aeabi_l2f", |c| c.set_ret32((r01(c) as i64 as f32).to_bits()));
    reg_raw("__aeabi_ul2f", |c| c.set_ret32((r01(c) as f32).to_bits()));
    // Rust's float-to-integer casts saturate and turn NaN into 0, as these helpers must.
    reg_raw("__aeabi_d2iz", |c| c.set_ret32(d0(c) as i32 as u32));
    reg_raw("__aeabi_d2uiz", |c| c.set_ret32(d0(c) as u32));
    reg_raw("__aeabi_f2iz", |c| c.set_ret32(f0(c) as i32 as u32));
    reg_raw("__aeabi_f2uiz", |c| c.set_ret32(f0(c) as u32));
    reg_raw("__aeabi_d2lz", |c| c.set_ret64(d0(c) as i64 as u64));
    reg_raw("__aeabi_d2ulz", |c| c.set_ret64(d0(c) as u64));
    reg_raw("__aeabi_f2lz", |c| c.set_ret64(f0(c) as i64 as u64));
    reg_raw("__aeabi_f2ulz", |c| c.set_ret64(f0(c) as u64));
}
