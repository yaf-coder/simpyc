/*
 * AArch64 (ARM64) context switch for simpyc.
 *
 * AAPCS callee-saved: x19-x28, x29 (fp), x30 (lr), sp, d8-d15.
 * Stack must remain 16-byte aligned. macOS-style symbols (leading _).
 *
 * Slot layout in coro_ctx_t:
 *   [0]  = sp
 *   [1]  = lr (x30)        <- return target on resume
 *   [2]  = x19  (used as arg slot for trampoline)
 *   [3]  = x20  (used as entry-fn slot for trampoline)
 *   [4]  = x21
 *   [5]  = x22
 *   ...
 *   [11] = x28
 *   [12] = x29 (fp)
 *   [13] = d8     ... [20] = d15
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

/* coro_switch(from, to) -- x0 = from, x1 = to */
        .globl SYM(coro_switch)
SYM(coro_switch):
        mov     x16, sp
        stp     x16, x30, [x0, #0]
        stp     x19, x20, [x0, #16]
        stp     x21, x22, [x0, #32]
        stp     x23, x24, [x0, #48]
        stp     x25, x26, [x0, #64]
        stp     x27, x28, [x0, #80]
        str     x29,      [x0, #96]
        stp     d8,  d9,  [x0, #112]
        stp     d10, d11, [x0, #128]
        stp     d12, d13, [x0, #144]
        stp     d14, d15, [x0, #160]

        ldp     x16, x30, [x1, #0]
        mov     sp, x16
        ldp     x19, x20, [x1, #16]
        ldp     x21, x22, [x1, #32]
        ldp     x23, x24, [x1, #48]
        ldp     x25, x26, [x1, #64]
        ldp     x27, x28, [x1, #80]
        ldr     x29,      [x1, #96]
        ldp     d8,  d9,  [x1, #112]
        ldp     d10, d11, [x1, #128]
        ldp     d12, d13, [x1, #144]
        ldp     d14, d15, [x1, #160]
        ret

/* Entered with x19=arg, x20=entry. Calls entry(arg); traps if it returns. */
        .globl SYM(coro_trampoline)
SYM(coro_trampoline):
        mov     x0, x19
        blr     x20
        brk     #0
