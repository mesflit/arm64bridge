//! ARM64Bridge runtime: runs the 32-bit ARM native libraries of an Android app inside a
//! 64-bit process. dynarmic recompiles the guest code; this crate loads the libraries,
//! provides the guest address space and translates every call between guest and host.
// SPDX-License-Identifier: GPL-2.0-or-later
#![allow(clippy::missing_safety_doc, clippy::too_many_arguments, clippy::type_complexity)]

#[macro_use]
pub mod log;
pub mod util;

pub mod api;
pub mod cpu;
pub mod crash;
pub mod elf;
pub mod frame_cap;
pub mod guest_types;
pub mod java_proc_hook;
pub mod jni;
pub mod limiter_probe;
pub mod mem;
pub mod proc;
pub mod signals;
pub mod thunk;
pub mod thunks;
pub mod trampoline;
pub mod zip;
pub mod zsys;

pub const VERSION: &str = env!("A64B_VERSION");
