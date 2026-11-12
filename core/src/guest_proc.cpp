// SPDX-License-Identifier: GPL-2.0-or-later
//
// Files under /proc that must look like a 32-bit ARM system to guest code.
//
// /proc/cpuinfo of an arm64 kernel lists arm64 features ("fp asimd ...") and no "Processor" line.
// Guest runtimes read it to choose their code: Mono (Unity) aborts with "thumb_supported not met"
// when the Features line has no "thumb", and others look for "neon" or "(v7". Guest code gets the
// cpuinfo a 32-bit process sees on an ARMv8 kernel, with the host's number of processors. The
// listed features are those the emulated CPU implements.

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "common.h"
#include "thunk.h"

namespace a64b {
namespace {

std::string cpuinfo_text() {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n < 1) n = 1;
    std::string s = "Processor\t: ARMv7 Processor rev 4 (v7l)\n";
    for (long i = 0; i < n; i++) {
        s += strfmt("processor\t: %ld\n", i);
        s += "model name\t: ARMv7 Processor rev 4 (v7l)\n"
             "BogoMIPS\t: 38.40\n"
             "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae evtstrm\n"
             "CPU implementer\t: 0x41\n"
             "CPU architecture: 7\n"
             "CPU variant\t: 0x0\n"
             "CPU part\t: 0xd03\n"
             "CPU revision\t: 4\n\n";
    }
    s += "Hardware\t: ARM64Bridge\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
    return s;
}

}  // namespace

int guest_proc_open(const char* path) {
    if (!path || strcmp(path, "/proc/cpuinfo") != 0) return -2;
    static const std::string text = cpuinfo_text();
    int fd = (int)syscall(SYS_memfd_create, "cpuinfo", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (write(fd, text.data(), text.size()) != (ssize_t)text.size() || lseek(fd, 0, SEEK_SET) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

}  // namespace a64b
