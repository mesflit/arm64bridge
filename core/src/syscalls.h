// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common.h"

namespace a64b {
struct Cpu;
// Raw "svc 0" system call issued by guest code (ARM EABI numbers).
u32 guest_syscall(Cpu& c, u32 nr);
u32 guest_syscall_args(Cpu& c, u32 nr, const u32 a[6]);
}  // namespace a64b
