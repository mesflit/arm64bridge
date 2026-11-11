// Host entry stubs for guest native methods.
//
// ART (or any JVM) calls a native method through a plain function pointer.
// Every stub in the pool below loads its own index and jumps to a common entry
// that spills the argument registers and calls a64b_tramp_dispatch(index,
// saved_registers, stack_arguments). The pool is static code, so no executable
// memory has to be generated at run time.
// SPDX-License-Identifier: GPL-2.0-or-later

#define E1(n) STUB(n)
#define E2(n) E1(n) E1((n) + 1)
#define E4(n) E2(n) E2((n) + 2)
#define E8(n) E4(n) E4((n) + 4)
#define E16(n) E8(n) E8((n) + 8)
#define E32(n) E16(n) E16((n) + 16)
#define E64(n) E32(n) E32((n) + 32)
#define E128(n) E64(n) E64((n) + 64)
#define E256(n) E128(n) E128((n) + 128)
#define E512(n) E256(n) E256((n) + 256)
#define E1024(n) E512(n) E512((n) + 512)
#define E2048(n) E1024(n) E1024((n) + 1024)
#define E4096(n) E2048(n) E2048((n) + 2048)

    .text

#if defined(__aarch64__)

#define STUB(n) mov w16, #(n) ; b a64b_tramp_common ;

    .balign 8
    .globl a64b_tramp_pool
    .type a64b_tramp_pool, %function
a64b_tramp_pool:
    E4096(0)
    .size a64b_tramp_pool, . - a64b_tramp_pool

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

#elif defined(__x86_64__)

#define STUB(n) .balign 16 ; movl $(n), %r10d ; jmp a64b_tramp_common ;

    .balign 16
    .globl a64b_tramp_pool
    .type a64b_tramp_pool, @function
a64b_tramp_pool:
    E4096(0)
    .size a64b_tramp_pool, . - a64b_tramp_pool

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

#else
#error "unsupported host architecture"
#endif

    .section .note.GNU-stack,"",%progbits
