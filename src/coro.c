/* Architecture-independent piece of the coroutine library.
 * coro_init wires up a saved-context so the next switch begins inside
 * coro_trampoline, which then calls entry(arg). */

#include "coro.h"

#include <stdint.h>
#include <string.h>

extern void coro_trampoline(void);

#if defined(__aarch64__) || defined(_M_ARM64)
/* arm64 layout: see coro_arm64.S */
void coro_init(coro_ctx_t *ctx, void *stack, size_t size,
               void (*entry)(void *), void *arg)
{
    memset(ctx, 0, sizeof(*ctx));
    uintptr_t top = (uintptr_t)stack + size;
    top &= ~(uintptr_t)15;                   /* 16-byte align */
    ctx->slots[0] = top;                     /* sp */
    ctx->slots[1] = (uintptr_t)coro_trampoline; /* lr */
    ctx->slots[2] = (uintptr_t)arg;          /* x19 */
    ctx->slots[3] = (uintptr_t)entry;        /* x20 */
}

#elif defined(__x86_64__) || defined(_M_X64)
/* x86_64 layout: see coro_amd64.S */
void coro_init(coro_ctx_t *ctx, void *stack, size_t size,
               void (*entry)(void *), void *arg)
{
    memset(ctx, 0, sizeof(*ctx));
    uintptr_t top = (uintptr_t)stack + size;
    top &= ~(uintptr_t)15;                   /* 16-byte align */
    ctx->slots[0] = top;                     /* rsp */
    ctx->slots[2] = (uintptr_t)arg;          /* rbx */
    ctx->slots[3] = (uintptr_t)entry;        /* r12 */
    ctx->slots[7] = (uintptr_t)coro_trampoline; /* rip */
}

#else
#  error "simpyc: unsupported architecture for coro_switch"
#endif
