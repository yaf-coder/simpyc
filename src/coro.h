/* Minimal stack-switching coroutine.
 *
 * Saves only callee-saved registers across a switch; ~10x faster than
 * ucontext (no signal mask syscall). Implemented in assembly for the
 * supported architectures. */
#ifndef SIMPYC_CORO_H
#define SIMPYC_CORO_H

#include <stddef.h>
#include <stdint.h>

/*  Slot count covers the largest backend (arm64: sp, lr, x19-x29 + d8-d15
 *  = 12 GPRs + 8 FPRs = 20 slots of 8 bytes). 32 slots gives headroom. */
typedef struct coro_ctx {
    uint64_t slots[32];
} coro_ctx_t;

/* Switch from `from` to `to`. The current execution state is saved into
 * `from`; execution resumes in `to`. */
void coro_switch(coro_ctx_t *from, coro_ctx_t *to);

/* Initialize `ctx` so a subsequent coro_switch(_, ctx) will begin
 * executing `entry(arg)` on the supplied stack. `stack` must point to
 * the LOW address of a stack region of `size` bytes (already malloc'd).
 * `entry` must not return; if it does, behavior is undefined.  */
void coro_init(coro_ctx_t *ctx, void *stack, size_t size,
               void (*entry)(void *arg), void *arg);

#endif
