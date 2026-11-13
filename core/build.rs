// Generates the pool of host entry stubs for guest native methods (see src/trampoline.rs).
// SPDX-License-Identifier: GPL-2.0-or-later
use std::fmt::Write;

const POOL: u32 = 4096;

fn main() {
    let out = std::env::var("OUT_DIR").unwrap();
    let arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    let mut s = String::new();
    s.push_str("    .text\n");
    match arch.as_str() {
        "aarch64" => {
            s.push_str("    .balign 8\n    .globl a64b_tramp_pool\n    .type a64b_tramp_pool, %function\na64b_tramp_pool:\n");
            for n in 0..POOL {
                writeln!(s, "    mov w16, #{n}\n    b a64b_tramp_common").unwrap();
            }
            s.push_str(
                "    .size a64b_tramp_pool, . - a64b_tramp_pool
    .balign 16
    .type a64b_tramp_common, %function
a64b_tramp_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #128
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp d0, d1, [sp, #64]
    stp d2, d3, [sp, #80]
    stp d4, d5, [sp, #96]
    stp d6, d7, [sp, #112]
    mov w0, w16
    mov x1, sp
    add x2, x29, #16
    bl a64b_tramp_dispatch
    fmov d0, x0
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
    .size a64b_tramp_common, . - a64b_tramp_common
",
            );
        }
        "x86_64" => {
            s.push_str("    .balign 16\n    .globl a64b_tramp_pool\n    .type a64b_tramp_pool, @function\na64b_tramp_pool:\n");
            for n in 0..POOL {
                writeln!(s, "    .balign 16\n    movl ${n}, %r10d\n    jmp a64b_tramp_common").unwrap();
            }
            s.push_str(
                "    .size a64b_tramp_pool, . - a64b_tramp_pool
    .balign 16
    .type a64b_tramp_common, @function
a64b_tramp_common:
    pushq %rbp
    movq %rsp, %rbp
    subq $112, %rsp
    movq %rdi, 0(%rsp)
    movq %rsi, 8(%rsp)
    movq %rdx, 16(%rsp)
    movq %rcx, 24(%rsp)
    movq %r8, 32(%rsp)
    movq %r9, 40(%rsp)
    movsd %xmm0, 48(%rsp)
    movsd %xmm1, 56(%rsp)
    movsd %xmm2, 64(%rsp)
    movsd %xmm3, 72(%rsp)
    movsd %xmm4, 80(%rsp)
    movsd %xmm5, 88(%rsp)
    movsd %xmm6, 96(%rsp)
    movsd %xmm7, 104(%rsp)
    movl %r10d, %edi
    movq %rsp, %rsi
    leaq 16(%rbp), %rdx
    call a64b_tramp_dispatch@PLT
    movq %rax, %xmm0
    leave
    ret
    .size a64b_tramp_common, . - a64b_tramp_common
",
            );
        }
        other => panic!("unsupported host architecture {other}"),
    }
    std::fs::write(format!("{out}/trampoline.s"), s).unwrap();
    #[cfg(feature = "dynarmic")]
    build_dynarmic_glue();
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=A64B_VERSION");
    println!("cargo:rustc-env=A64B_VERSION={}", std::env::var("A64B_VERSION").unwrap_or_else(|_| "dev".into()));
}

// Temporary C++ adapter for dynarmic (feature "dynarmic"). A64B_DYNARMIC_DIR is a CMake build
// directory in which dynarmic was fetched and built (it holds _deps/dynarmic-src and -build).
#[cfg(feature = "dynarmic")]
fn build_dynarmic_glue() {
    let dir = std::env::var("A64B_DYNARMIC_DIR").expect("feature dynarmic needs A64B_DYNARMIC_DIR");
    let src = format!("{dir}/_deps/dynarmic-src");
    cc::Build::new()
        .cpp(true)
        .std("c++20")
        .file("glue/dynarmic_glue.cpp")
        .include(format!("{src}/src"))
        .include(format!("{src}/externals/mcl/include"))
        .flag_if_supported("-Wno-unused-parameter")
        .compile("a64b_dynarmic_glue");
    let b = format!("{dir}/_deps/dynarmic-build");
    for (d, l) in [("src/dynarmic", "dynarmic"), ("externals/mcl/src", "mcl"), ("externals/fmt", "fmt"), ("externals/zydis", "Zydis"), ("externals/zydis/zycore", "Zycore")] {
        println!("cargo:rustc-link-search=native={b}/{d}");
        println!("cargo:rustc-link-lib=static={l}");
    }
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rerun-if-changed=glue/dynarmic_glue.cpp");
}
