//! POSIX / bionic functions whose structures differ between the 32-bit guest and
//! the 64-bit host (epoll, msghdr, iovec, statfs, ifreq, passwd, div_t...) and
//! wide-character helpers.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::cell::Cell;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};

use crate::cpu::BlockingCall;
use crate::guest_types::*;
use crate::mem::*;
use crate::thunk::{reg, reg_raw, ArgReader, VaSource};
use crate::thunks::libc::{intern_cstr, sys};
use crate::thunks::stdio::{guest_file_get, guest_format};
use crate::util::{cstr, set_errno};

type Str = *const c_char;

extern "C" {
    fn inet_ntoa(a: libc::in_addr) -> *const c_char;
    fn inet_aton(s: *const c_char, a: *mut libc::in_addr) -> c_int;
    fn inet_ntop(af: c_int, src: *const c_void, dst: *mut c_char, n: u32) -> *const c_char;
    fn inet_pton(af: c_int, src: *const c_char, dst: *mut c_void) -> c_int;
    fn iswblank(c: u32) -> c_int;
    fn iswgraph(c: u32) -> c_int;
    fn fgetwc(f: *mut libc::FILE) -> u32;
    fn fputwc(wc: u32, f: *mut libc::FILE) -> u32;
    fn ungetwc(wc: u32, f: *mut libc::FILE) -> u32;
    fn strptime(s: *const c_char, fmt: *const c_char, tm: *mut libc::tm) -> *mut c_char;
}

// ---- epoll: ARM32 struct epoll_event is { u32 events; u32 pad; u64 data } ----
#[repr(C)]
#[derive(Clone, Copy)]
struct GEpollEvent {
    events: u32,
    pad: u32,
    data: u64,
}
const _: () = assert!(std::mem::size_of::<GEpollEvent>() == 16);

// ---- iovec / msghdr ----
#[repr(C)]
#[derive(Clone, Copy)]
struct GMsghdr {
    name: u32,
    namelen: u32,
    iov: u32,
    iovlen: u32,
    control: u32,
    controllen: u32,
    flags: i32,
}

unsafe fn iov_from_guest(giov: u32, n: u32) -> Vec<libc::iovec> {
    (0..n).map(|i| libc::iovec { iov_base: g2h(rd::<u32>(giov + i * 8)), iov_len: rd::<u32>(giov + i * 8 + 4) as usize }).collect()
}

// ---- statfs (bionic LP32 layout) ----
#[repr(C)]
struct GStatfs {
    f_type: u32,
    f_bsize: u32,
    f_blocks: u64,
    f_bfree: u64,
    f_bavail: u64,
    f_files: u64,
    f_ffree: u64,
    f_fsid: [i32; 2],
    f_namelen: u32,
    f_frsize: u32,
    f_flags: u32,
    f_spare: [u32; 4],
    pad: u32,
}
const _: () = assert!(std::mem::size_of::<GStatfs>() == 88);

unsafe fn statfs_to_guest(g: *mut GStatfs, s: &libc::statfs) {
    let mut v: GStatfs = std::mem::zeroed();
    v.f_type = s.f_type as u32;
    v.f_bsize = s.f_bsize as u32;
    v.f_blocks = s.f_blocks as u64;
    v.f_bfree = s.f_bfree as u64;
    v.f_bavail = s.f_bavail as u64;
    v.f_files = s.f_files as u64;
    v.f_ffree = s.f_ffree as u64;
    std::ptr::copy_nonoverlapping(&s.f_fsid as *const _ as *const u8, v.f_fsid.as_mut_ptr() as *mut u8, 8);
    v.f_namelen = s.f_namelen as u32;
    v.f_frsize = s.f_frsize as u32;
    v.f_flags = 0; // ST_* mount flags: not in the host's struct statfs binding
    // The guest structure is 88 bytes with 8-byte alignment; write exactly its size.
    std::ptr::copy_nonoverlapping(&v as *const GStatfs as *const u8, g as *mut u8, 88);
}

// ---- ifreq: 32 bytes in the guest, 40 on the host (its union holds pointers) ----
const GUEST_IFREQ: usize = 32;
const HOST_IFREQ: usize = 40;
const SIOCGIFCONF: u32 = 0x8912;

#[repr(C)]
struct HostIfconf {
    len: c_int,
    buf: *mut u8,
}

unsafe fn socket_ioctl(fd: i32, req: u32, arg: u32) -> i32 {
    if req == SIOCGIFCONF {
        // struct ifconf { int len; char* buf; }
        let (glen, gbuf) = (rd::<u32>(arg), rd::<u32>(arg + 4));
        let n = glen as usize / GUEST_IFREQ;
        let mut tmp = vec![0u8; n.max(1) * HOST_IFREQ];
        let mut hc = HostIfconf { len: (n * HOST_IFREQ) as c_int, buf: if gbuf != 0 { tmp.as_mut_ptr() } else { std::ptr::null_mut() } };
        let r = libc::ioctl(fd, SIOCGIFCONF as _, &mut hc as *mut HostIfconf);
        if r < 0 {
            return r;
        }
        let got = hc.len as usize / HOST_IFREQ;
        if gbuf != 0 {
            for i in 0..got.min(n) {
                std::ptr::copy_nonoverlapping(tmp.as_ptr().add(i * HOST_IFREQ), g2h::<u8>(gbuf).add(i * GUEST_IFREQ), GUEST_IFREQ);
            }
        }
        wr::<u32>(arg, (got * GUEST_IFREQ) as u32);
        return r;
    }
    if (req & 0xFF00) == 0x8900 && arg != 0 {
        // Other interface requests take a single struct ifreq (name + sockaddr/int/short).
        let mut hr = [0u8; HOST_IFREQ];
        std::ptr::copy_nonoverlapping(g2h::<u8>(arg), hr.as_mut_ptr(), GUEST_IFREQ);
        let r = libc::ioctl(fd, req as _, hr.as_mut_ptr());
        std::ptr::copy_nonoverlapping(hr.as_ptr(), g2h::<u8>(arg), GUEST_IFREQ);
        return r;
    }
    libc::ioctl(fd, req as _, arg as usize)
}

// ---- wide characters ----
unsafe fn narrow(mut w: *const u32) -> Vec<u8> {
    let mut s = Vec::new();
    if w.is_null() {
        return s;
    }
    loop {
        let ch = w.read_unaligned();
        if ch == 0 {
            break;
        }
        s.push(if ch < 128 { ch as u8 } else { b'?' });
        w = w.add(1);
    }
    s
}

unsafe fn widen_into(out: *mut u32, cap: u32, s: &[u8]) -> i32 {
    if cap == 0 {
        return -1;
    }
    // Decode UTF-8 produced by the formatter into code points.
    let mut n = 0u32;
    let mut i = 0;
    while i < s.len() && n + 1 < cap {
        let ch = s[i];
        let mut cp = ch as u32;
        let extra = if ch >= 0xF0 { 3 } else if ch >= 0xE0 { 2 } else if ch >= 0xC0 { 1 } else { 0 };
        if extra != 0 {
            cp = (ch & (0x3F >> extra)) as u32;
        }
        i += 1;
        let mut k = 0;
        while k < extra && i < s.len() {
            cp = (cp << 6) | (s[i] & 0x3F) as u32;
            k += 1;
            i += 1;
        }
        out.add(n as usize).write_unaligned(cp);
        n += 1;
    }
    out.add(n as usize).write_unaligned(0);
    n as i32
}

const WCTYPE_NAMES: [&str; 12] = ["alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"];

unsafe fn wctype_test(wc: u32, t: u32) -> i32 {
    match t {
        1 => sys::iswalnum(wc),
        2 => sys::iswalpha(wc),
        3 => iswblank(wc),
        4 => sys::iswcntrl(wc),
        5 => sys::iswdigit(wc),
        6 => iswgraph(wc),
        7 => sys::iswlower(wc),
        8 => sys::iswprint(wc),
        9 => sys::iswpunct(wc),
        10 => sys::iswspace(wc),
        11 => sys::iswupper(wc),
        12 => sys::iswxdigit(wc),
        _ => 0,
    }
}

static INET_BUF: AtomicU32 = AtomicU32::new(0);
static TMPNAM_BUF: AtomicU32 = AtomicU32::new(0);
static PASSWD: AtomicU32 = AtomicU32::new(0);
static TMPNAM_COUNTER: AtomicI32 = AtomicI32::new(0);

fn lazy(slot: &AtomicU32, make: impl FnOnce() -> u32) -> u32 {
    let mut v = slot.load(Ordering::Relaxed);
    if v == 0 {
        v = make();
        slot.store(v, Ordering::Relaxed);
    }
    v
}

unsafe fn put_cstr(dst: u32, cap: usize, s: &[u8]) {
    let n = s.len().min(cap - 1);
    std::ptr::copy_nonoverlapping(s.as_ptr(), g2h::<u8>(dst), n);
    wr::<u8>(dst + n as u32, 0);
}

fn passwd() -> u32 {
    lazy(&PASSWD, || unsafe {
        let p = g_calloc(6, 4);
        wr::<u32>(p, g_strdup_bytes(b"app"));
        wr::<u32>(p + 4, 0);
        wr::<u32>(p + 8, libc::getuid());
        wr::<u32>(p + 12, libc::getgid());
        wr::<u32>(p + 16, g_strdup_bytes(b"/"));
        wr::<u32>(p + 20, g_strdup_bytes(b"/system/bin/sh"));
        p
    })
}

unsafe fn epoll_wait_guest(epfd: i32, out: *mut GEpollEvent, max: i32, timeout: i32) -> i32 {
    let _blocking = BlockingCall::new();
    if max <= 0 {
        set_errno(libc::EINVAL);
        return -1;
    }
    let mut he: Vec<libc::epoll_event> = vec![libc::epoll_event { events: 0, u64: 0 }; max as usize];
    let n = libc::epoll_wait(epfd, he.as_mut_ptr(), max, timeout);
    for i in 0..n.max(0) as usize {
        let e = he[i];
        out.add(i).write_unaligned(GEpollEvent { events: e.events, pad: 0, data: e.u64 });
    }
    n
}

unsafe fn msg_call(fd: i32, g: *mut GMsghdr, flags: i32, send: bool) -> isize {
    let mut gm = g.read_unaligned();
    let mut v = iov_from_guest(gm.iov, gm.iovlen);
    let mut h: libc::msghdr = std::mem::zeroed();
    h.msg_name = g2h(gm.name);
    h.msg_namelen = gm.namelen;
    h.msg_iov = v.as_mut_ptr();
    h.msg_iovlen = v.len() as _;
    if gm.controllen != 0 {
        static WARNED: AtomicBool = AtomicBool::new(false);
        if !WARNED.swap(true, Ordering::Relaxed) {
            logw!("sendmsg/recvmsg: ancillary data is not bridged and was dropped");
        }
    }
    let r = if send { libc::sendmsg(fd, &h, flags) } else { libc::recvmsg(fd, &mut h, flags) };
    if !send {
        gm.namelen = h.msg_namelen;
        gm.controllen = 0;
        gm.flags = h.msg_flags;
        g.write_unaligned(gm);
    }
    r
}

macro_rules! r {
    ($name:expr, |$($a:ident : $t:ty),*| -> $ret:ty $body:block) => {
        reg($name, move |$($a: $t),*| -> $ret { unsafe { $body } })
    };
    ($name:expr, |$($a:ident : $t:ty),*| $body:block) => {
        reg($name, move |$($a: $t),*| -> () { unsafe { $body } })
    };
}

pub fn register_posix_extra_thunks() {
    // ---------------------------------------------------------------- epoll / eventfd
    r!("epoll_create", |n: i32| -> i32 { libc::epoll_create(n.max(1)) });
    r!("epoll_create1", |flags: i32| -> i32 { libc::epoll_create1(flags) });
    r!("epoll_ctl", |epfd: i32, op: i32, fd: i32, ge: *const GEpollEvent| -> i32 {
        let mut he = libc::epoll_event { events: 0, u64: 0 };
        if !ge.is_null() {
            let g = ge.read_unaligned();
            he.events = g.events;
            he.u64 = g.data;
        }
        libc::epoll_ctl(epfd, op, fd, if ge.is_null() { std::ptr::null_mut() } else { &mut he })
    });
    r!("epoll_wait", |epfd: i32, out: *mut GEpollEvent, max: i32, timeout: i32| -> i32 { epoll_wait_guest(epfd, out, max, timeout) });
    r!("epoll_pwait", |epfd: i32, out: *mut GEpollEvent, max: i32, timeout: i32, _mask: u32| -> i32 { epoll_wait_guest(epfd, out, max, timeout) });
    r!("eventfd", |init: u32, flags: i32| -> i32 { libc::eventfd(init, flags) });
    r!("eventfd_read", |fd: i32, v: *mut u64| -> i32 {
        let mut x = 0u64;
        if libc::read(fd, &mut x as *mut u64 as *mut c_void, 8) != 8 {
            return -1;
        }
        v.write_unaligned(x);
        0
    });
    r!("eventfd_write", |fd: i32, v: u64| -> i32 { if libc::write(fd, &v as *const u64 as *const c_void, 8) == 8 { 0 } else { -1 } });

    // ---------------------------------------------------------------- scatter/gather I/O
    r!("readv", |fd: i32, giov: u32, n: i32| -> isize {
        let v = iov_from_guest(giov, n.max(0) as u32);
        libc::readv(fd, v.as_ptr(), n)
    });
    r!("writev", |fd: i32, giov: u32, n: i32| -> isize {
        let v = iov_from_guest(giov, n.max(0) as u32);
        libc::writev(fd, v.as_ptr(), n)
    });
    r!("sendmsg", |fd: i32, g: *mut GMsghdr, flags: i32| -> isize { msg_call(fd, g, flags, true) });
    r!("recvmsg", |fd: i32, g: *mut GMsghdr, flags: i32| -> isize { msg_call(fd, g, flags, false) });

    reg_raw("ioctl", |c| unsafe {
        let mut a = ArgReader::new(c);
        let (fd, req, arg) = (a.i32(), a.next32(), a.next32());
        c.set_ret32(socket_ioctl(fd, req, arg) as u32);
    });

    // ---------------------------------------------------------------- file systems
    for n in ["statfs", "statfs64"] {
        r!(n, |p: Str, g: *mut GStatfs| -> i32 {
            let mut s: libc::statfs = std::mem::zeroed();
            let r = libc::statfs(p, &mut s);
            if r == 0 {
                statfs_to_guest(g, &s);
            }
            r
        });
    }
    for n in ["fstatfs", "fstatfs64"] {
        r!(n, |fd: i32, g: *mut GStatfs| -> i32 {
            let mut s: libc::statfs = std::mem::zeroed();
            let r = libc::fstatfs(fd, &mut s);
            if r == 0 {
                statfs_to_guest(g, &s);
            }
            r
        });
    }
    // statvfs has the same field order in bionic LP32 (f_bsize, f_frsize, then 64-bit counters).
    // statvfs has the same field order in bionic LP32 (f_bsize, f_frsize, then 64-bit counters).
    r!("statvfs", |p: Str, g: u32| -> i32 {
        let mut s: libc::statvfs = std::mem::zeroed();
        let r = libc::statvfs(p, &mut s);
        if r != 0 {
            return r;
        }
        std::ptr::write_bytes(g2h::<u8>(g), 0, 96);
        wr::<u32>(g, s.f_bsize as u32);
        wr::<u32>(g + 4, s.f_frsize as u32);
        for (i, v) in [s.f_blocks as u64, s.f_bfree as u64, s.f_bavail as u64, s.f_files as u64, s.f_ffree as u64, s.f_favail as u64].into_iter().enumerate() {
            wr::<u64>(g + 8 + i as u32 * 8, v);
        }
        wr::<u32>(g + 56, 0); // f_fsid
        wr::<u32>(g + 60, s.f_flag as u32); // f_flag
        wr::<u32>(g + 64, s.f_namemax as u32); // f_namemax
        0
    });
    r!("tmpnam", |out: *mut c_char| -> u32 {
        let d = if out.is_null() { lazy(&TMPNAM_BUF, || g_calloc(1, 256)) } else { h2g(out) };
        let base = std::env::var("TMPDIR").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| "/data/local/tmp".into());
        let name = format!("{}/a64b-{}-{}", base, libc::getpid(), TMPNAM_COUNTER.fetch_add(1, Ordering::Relaxed));
        put_cstr(d, 256, name.as_bytes());
        d
    });

    // ---------------------------------------------------------------- network names
    r!("gethostname", |buf: *mut c_char, n: usize| -> i32 { libc::gethostname(buf, n) });
    // struct utsname is six 65-byte strings on every ABI. Report the machine a 32-bit process would see.
    r!("uname", |u: *mut libc::utsname| -> i32 {
        let r = libc::uname(u);
        if r == 0 {
            let m = b"armv7l\0";
            std::ptr::copy_nonoverlapping(m.as_ptr(), (*u).machine.as_mut_ptr() as *mut u8, m.len());
        }
        r
    });
    r!("inet_ntoa", |addr: u32| -> u32 {
        let buf = lazy(&INET_BUF, || g_calloc(1, 32));
        put_cstr(buf, 32, crate::util::cbytes(inet_ntoa(libc::in_addr { s_addr: addr })));
        buf
    });
    r!("inet_aton", |s: Str, out: *mut u32| -> i32 {
        let mut a = libc::in_addr { s_addr: 0 };
        let r = inet_aton(s, &mut a);
        if r != 0 && !out.is_null() {
            out.write_unaligned(a.s_addr);
        }
        r
    });
    r!("inet_ntop", |af: i32, src: *const c_void, dst: *mut c_char, n: u32| -> *const c_char { inet_ntop(af, src, dst, n) });
    r!("inet_pton", |af: i32, src: Str, dst: *mut c_void| -> i32 { inet_pton(af, src, dst) });
    reg("gethostbyaddr", |_a: u32, _l: u32, _t: i32| -> u32 { 0 });
    // struct addrinfo (LP32): flags, family, socktype, protocol, addrlen, canonname, addr, next
    r!("getaddrinfo", |node: Str, service: Str, ghints: *const i32, out: *mut u32| -> i32 {
        let mut hints: libc::addrinfo = std::mem::zeroed();
        let mut res: *mut libc::addrinfo = std::ptr::null_mut();
        if !ghints.is_null() {
            hints.ai_flags = *ghints;
            hints.ai_family = *ghints.add(1);
            hints.ai_socktype = *ghints.add(2);
            hints.ai_protocol = *ghints.add(3);
        }
        let r = libc::getaddrinfo(node, service, if ghints.is_null() { std::ptr::null() } else { &hints }, &mut res);
        logi!("getaddrinfo({}, {}) = {}", cstr(node), cstr(service), r);
        if r != 0 {
            return r;
        }
        let mut head = 0u32;
        let mut link = 0u32; // guest address of the previous entry's "next" field
        let mut ai = res;
        while !ai.is_null() {
            let alen = (*ai).ai_addrlen as u32;
            let g = g_calloc(1, 32 + alen);
            wr::<u32>(g, (*ai).ai_flags as u32);
            wr::<u32>(g + 4, (*ai).ai_family as u32);
            wr::<u32>(g + 8, (*ai).ai_socktype as u32);
            wr::<u32>(g + 12, (*ai).ai_protocol as u32);
            wr::<u32>(g + 16, alen);
            wr::<u32>(g + 20, if (*ai).ai_canonname.is_null() { 0 } else { g_strdup((*ai).ai_canonname) });
            wr::<u32>(g + 24, g + 32);
            std::ptr::copy_nonoverlapping((*ai).ai_addr as *const u8, g2h::<u8>(g + 32), alen as usize);
            wr::<u32>(g + 28, 0);
            if link == 0 {
                head = g;
            } else {
                wr::<u32>(link, g);
            }
            link = g + 28;
            ai = (*ai).ai_next;
        }
        libc::freeaddrinfo(res);
        *out = head;
        0
    });
    r!("freeaddrinfo", |g: u32| {
        let mut g = g;
        while g != 0 {
            let next = rd::<u32>(g + 28);
            let canon = rd::<u32>(g + 20);
            if canon != 0 {
                g_free(canon);
            }
            g_free(g);
            g = next;
        }
    });
    r!("gai_strerror", |e: i32| -> u32 { intern_cstr(libc::gai_strerror(e)) });
    // struct hostent (LP32): name, aliases, addrtype, length, addr_list. One buffer per thread.
    r!("gethostbyname", |name: Str| -> u32 {
        let mut hints: libc::addrinfo = std::mem::zeroed();
        let mut res: *mut libc::addrinfo = std::ptr::null_mut();
        hints.ai_family = libc::AF_INET;
        let r = libc::getaddrinfo(name, std::ptr::null(), &hints, &mut res);
        logi!("gethostbyname({}) = {}", cstr(name), r);
        if r != 0 || res.is_null() {
            return 0;
        }
        thread_local! { static BUF: Cell<u32> = const { Cell::new(0) }; }
        let buf = BUF.with(|b| {
            if b.get() == 0 {
                b.set(g_calloc(1, 64 + 16 * 4 + 16 * 4 + 256));
            }
            b.get()
        });
        let (addrs, list, name_g) = (buf + 64, buf + 64 + 64, buf + 64 + 128);
        put_cstr(name_g, 256, crate::util::cbytes(name));
        let mut n = 0u32;
        let mut ai = res;
        while !ai.is_null() && n < 15 {
            if (*ai).ai_family == libc::AF_INET {
                let sin = (*ai).ai_addr as *const libc::sockaddr_in;
                wr::<u32>(addrs + n * 4, (*sin).sin_addr.s_addr);
                wr::<u32>(list + n * 4, addrs + n * 4);
                n += 1;
            }
            ai = (*ai).ai_next;
        }
        wr::<u32>(list + n * 4, 0);
        libc::freeaddrinfo(res);
        wr::<u32>(buf, name_g);
        wr::<u32>(buf + 4, list + n * 4); // empty alias list (points at the terminating NULL)
        wr::<u32>(buf + 8, libc::AF_INET as u32);
        wr::<u32>(buf + 12, 4);
        wr::<u32>(buf + 16, list);
        if n != 0 { buf } else { 0 }
    });

    // ---------------------------------------------------------------- users
    reg("getpwuid", |_uid: u32| -> u32 { passwd() });
    reg("getpwnam", |_name: u32| -> u32 { passwd() });
    reg("getlogin", || -> u32 { unsafe { rd::<u32>(passwd()) } });

    // ---------------------------------------------------------------- div_t / ldiv_t: returned in memory (r0 = result)
    for name in ["div", "ldiv"] {
        reg_raw(name, |c| unsafe {
            let (n, d) = (c.r(1) as i32, c.r(2) as i32);
            let out = c.r(0);
            wr::<u32>(out, if d != 0 { n.wrapping_div(d) as u32 } else { 0 });
            wr::<u32>(out + 4, if d != 0 { n.wrapping_rem(d) as u32 } else { 0 });
        });
    }
    reg_raw("lldiv", |c| unsafe {
        // r0 = result, (r2,r3) = numerator, stack = denominator
        let mut a = ArgReader::new(c);
        let out = a.next32();
        let (n, d) = (a.next64() as i64, a.next64() as i64);
        wr::<i64>(out, if d != 0 { n.wrapping_div(d) } else { 0 });
        wr::<i64>(out + 8, if d != 0 { n.wrapping_rem(d) } else { 0 });
    });

    // ---------------------------------------------------------------- time parsing
    r!("strptime", |s: Str, fmt: Str, g: *mut GTm| -> *mut c_char {
        let gv = g.read_unaligned();
        let mut t = tm_from_guest(&gv);
        let end = strptime(s, fmt, &mut t);
        g.write_unaligned(tm_to_guest(&t, gv.tm_zone));
        end
    });

    // ---------------------------------------------------------------- wide characters
    r!("wctype", |name: Str| -> u32 {
        let n = cstr(name);
        WCTYPE_NAMES.iter().position(|&w| w == n).map(|i| i as u32 + 1).unwrap_or(0)
    });
    r!("iswctype", |wc: u32, t: u32| -> i32 { wctype_test(wc, t) });
    r!("wcsxfrm", |d: *mut u32, s: *const u32, n: u32| -> u32 {
        let len = sys::wcslen(s) as u32;
        if n > len {
            std::ptr::copy_nonoverlapping(s as *const u8, d as *mut u8, (len as usize + 1) * 4);
        }
        len
    });
    for (name, list) in [("swprintf", false), ("vswprintf", true)] {
        reg_raw(name, move |c| unsafe {
            let mut a = ArgReader::new(c);
            let out: *mut u32 = a.ptr();
            let cap = a.next32();
            let f = CString::new(narrow(a.ptr::<u32>())).unwrap_or_default();
            let mut va = if list { VaSource::from_va_list(a.next32()) } else { VaSource::from_args(&a) };
            let s = guest_format(f.as_ptr(), &mut va);
            let n = widen_into(out, cap, &s);
            c.set_ret32(if n >= 0 && (n as usize) < s.len() && n as u32 + 1 >= cap { -1 } else { n } as u32);
        });
    }
    r!("wcsftime", |out: *mut u32, cap: u32, fmt: *const u32, g: *const GTm| -> u32 {
        let t = tm_from_guest(&g.read_unaligned());
        let f = CString::new(narrow(fmt)).unwrap_or_default();
        let mut buf = [0u8; 512];
        let n = libc::strftime(buf.as_mut_ptr() as *mut c_char, buf.len(), f.as_ptr(), &t);
        if n == 0 || n >= cap as usize {
            return 0;
        }
        widen_into(out, cap, &buf[..n]) as u32
    });
    const WEOF: u32 = 0xFFFF_FFFF;
    for n in ["getwc", "fgetwc"] {
        r!(n, |g: u32| -> u32 {
            let f = guest_file_get(g);
            if f.is_null() { WEOF } else { fgetwc(f) }
        });
    }
    for n in ["putwc", "fputwc"] {
        r!(n, |wc: u32, g: u32| -> u32 {
            let f = guest_file_get(g);
            if f.is_null() { WEOF } else { fputwc(wc, f) }
        });
    }
    r!("ungetwc", |wc: u32, g: u32| -> u32 {
        let f = guest_file_get(g);
        if f.is_null() { WEOF } else { ungetwc(wc, f) }
    });
    r!("wcstol", |s: *const u32, end: *mut u32, base: i32| -> i32 {
        let n = CString::new(narrow(s)).unwrap_or_default();
        let mut e: *mut c_char = std::ptr::null_mut();
        let v = sys::strtol(n.as_ptr(), &mut e, base);
        if !end.is_null() {
            *end = h2g(s) + (e as usize - n.as_ptr() as usize) as u32 * 4;
        }
        v as i32
    });
    r!("wcstod", |s: *const u32, end: *mut u32| -> f64 {
        let n = CString::new(narrow(s)).unwrap_or_default();
        let mut e: *mut c_char = std::ptr::null_mut();
        let v = sys::strtod(n.as_ptr(), &mut e);
        if !end.is_null() {
            *end = h2g(s) + (e as usize - n.as_ptr() as usize) as u32 * 4;
        }
        v
    });
}
