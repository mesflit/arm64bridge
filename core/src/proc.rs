//! Files under /proc that must look like a 32-bit ARM system to guest code.
//!
//! /proc/cpuinfo of an arm64 kernel lists arm64 features ("fp asimd ...") and no "Processor" line.
//! Guest runtimes read it to choose their code: Mono (Unity) aborts with "thumb_supported not met"
//! when the Features line has no "thumb", and others look for "neon" or "(v7". Guest code gets the
//! cpuinfo a 32-bit process sees on an ARMv8 kernel, with the host's number of processors. The
//! listed features are those the emulated CPU implements.
// SPDX-License-Identifier: GPL-2.0-or-later
use std::os::raw::c_char;
use std::sync::OnceLock;

use crate::util::{errno, set_errno};

fn cpuinfo_text() -> String {
    let n = unsafe { libc::sysconf(libc::_SC_NPROCESSORS_CONF) }.max(1);
    let mut s = String::from("Processor\t: ARMv7 Processor rev 4 (v7l)\n");
    for i in 0..n {
        s += &format!("processor\t: {}\n", i);
        s += "model name\t: ARMv7 Processor rev 4 (v7l)\n\
              BogoMIPS\t: 38.40\n\
              Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae evtstrm\n\
              CPU implementer\t: 0x41\n\
              CPU architecture: 7\n\
              CPU variant\t: 0x0\n\
              CPU part\t: 0xd03\n\
              CPU revision\t: 4\n\n";
    }
    s += "Hardware\t: ARM64Bridge\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
    s
}

/// A /proc file shown differently to guest code: an open, read-only descriptor,
/// -1 with errno on failure, or -2 when the path is an ordinary file.
pub unsafe fn guest_proc_open(path: *const c_char) -> i32 {
    if path.is_null() || libc::strcmp(path, c"/proc/cpuinfo".as_ptr()) != 0 {
        return -2;
    }
    static TEXT: OnceLock<String> = OnceLock::new();
    let text = TEXT.get_or_init(cpuinfo_text);
    let fd = libc::syscall(libc::SYS_memfd_create, c"cpuinfo".as_ptr(), 1 /* MFD_CLOEXEC */) as i32;
    if fd < 0 {
        return -1;
    }
    if libc::write(fd, text.as_ptr() as *const _, text.len()) != text.len() as isize || libc::lseek(fd, 0, libc::SEEK_SET) != 0 {
        let e = errno();
        libc::close(fd);
        set_errno(e);
        return -1;
    }
    fd
}
