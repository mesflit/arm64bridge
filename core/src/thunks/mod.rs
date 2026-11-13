//! Host implementations of the functions guest libraries import from the system.
// SPDX-License-Identifier: GPL-2.0-or-later
pub mod android;
pub mod dl;
pub mod gles;
pub mod libc;
pub mod math;
pub mod misc;
pub mod old_bionic;
pub mod opensl;
pub mod posix_extra;
pub mod pthread;
pub mod stdio;
pub mod zlib;
