/*
 * x86_64 SysV context switch for simpyc.
 *
 * Callee-saved: rbx, rbp, r12-r15, rsp. Stack must be 16-byte aligned
 * just before the `call` instruction (so 8 mod 16 at function entry).
 *
 * Slot layout:
 *   [0] = rsp
 *   [1] = rbp
 *   [2] = rbx   (used as arg slot for trampoline)
 *   [3] = r12   (used as entry-fn slot for trampoline)
 *   [4] = r13
 *   [5] = r14
 *   [6] = r15
 *   [7] = rip   (return target on resume)
 */

#if defined(__APPLE__)
#  define SYM(x) _##x
#else
#  define SYM(x) x
#endif

#if defined(__APPLE__)
        .section __TEXT,__text,regular,pure_instructions
#else
        .text
#endif
        .align 4

/* coro_switch(from, to) -- rdi = from, rsi = to */
        .globl SYM(coro_switch)
SYM(coro_switch):
        popq    %rax              /* pop return address into rax */
        movq    %rsp,   0(%rdi)
        movq    %rax,  56(%rdi)
        movq    %rbp,   8(%rdi)
        movq    %rbx,  16(%rdi)
        movq    %r12,  24(%rdi)
        movq    %r13,  32(%rdi)
        movq    %r14,  40(%rdi)
        movq    %r15,  48(%rdi)

        movq     0(%rsi), %rsp
        movq    56(%rsi), %rax
        movq     8(%rsi), %rbp
        movq    16(%rsi), %rbx
        movq    24(%rsi), %r12
        movq    32(%rsi), %r13
        movq    40(%rsi), %r14
        movq    48(%rsi), %r15
        jmpq    *%rax

/* Entered with rbx=arg, r12=entry. Calls entry(arg); ud2 if it returns. */
        .globl SYM(coro_trampoline)
SYM(coro_trampoline):
        movq    %rbx, %rdi
        callq   *%r12
        ud2
