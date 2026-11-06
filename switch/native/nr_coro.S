/* MPH-nx coroutine switch. Saves and restores the callee-saved registers of
 * the host ABI and swaps stacks. aarch64 (Switch) and x86-64 (host tests). */
#if defined(__aarch64__)
    .text
    .global nr_coro_switch
    .type nr_coro_switch, %function
nr_coro_switch:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x2, sp
    str x2, [x0]
    ldr x2, [x1]
    mov sp, x2
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #176
    ret
    .size nr_coro_switch, . - nr_coro_switch

    .global nr_coro_trampoline
    .type nr_coro_trampoline, %function
nr_coro_trampoline:
    mov x0, x20
    blr x19
1:  b 1b
    .size nr_coro_trampoline, . - nr_coro_trampoline
#elif defined(__x86_64__)
    .text
    .global nr_coro_switch
    .type nr_coro_switch, @function
nr_coro_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq (%rsi), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size nr_coro_switch, . - nr_coro_switch

    .global nr_coro_trampoline
    .type nr_coro_trampoline, @function
nr_coro_trampoline:
    movq %r13, %rdi
    andq $-16, %rsp
    callq *%r12
1:  jmp 1b
    .size nr_coro_trampoline, . - nr_coro_trampoline
#endif
    .section .note.GNU-stack,"",%progbits
