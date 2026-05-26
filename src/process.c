/* Processes: each runs on its own stack via the coro library.
 *
 * Lifecycle:
 *   sim_process()           -> alloc + coro_init; immediately scheduled to run.
 *   first dispatch          -> trampoline runs user fn until it yields.
 *   sim_yield(self,e)       -> subscribe waiter_cb to e; switch out.
 *   waiter_cb fires         -> schedule_resume(p); pops; resume_cb switches in.
 *   sim_process_interrupt() -> schedule_resume(p) with cause, clear yielded.
 *   user fn returns         -> succeed `done`; switch out permanently.
 *
 * Each process owns a single reusable resume event (resume_ev). When a
 * resume is already pending we coalesce instead of allocating a new
 * event — keeps the per-yield allocation count at zero in steady state. */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* 16 KiB default; SimPy-style processes rarely need more than ~1 KiB.
 * Bigger stacks force malloc to take the slow path and inflate spawn
 * cost. If your process bodies use deep recursion or large stack
 * arrays, fork the library and bump this. */
#define SIM_DEFAULT_STACK   (16 * 1024)

static void schedule_resume(sim_process_t *p, void *value);

/* When the process's resume event fires, switch into the process. */
static void resume_cb(sim_event_t *ev, void *user) {
    (void)ev;
    sim_process_t *p = (sim_process_t *)user;
    if (p->state == SIM_PROC_DEAD) return;
    sim_env_t *env = p->env;
    env->active = p;
    coro_switch(&env->host_ctx, &p->ctx);
    env->active = NULL;
}

/* When a yielded event fires, schedule the waiting process for resume
 * with the event's value. Skip if the process has moved on (e.g. was
 * interrupted before this fired). */
static void waiter_cb(sim_event_t *ev, void *user) {
    sim_process_t *p = (sim_process_t *)user;
    if (p->state == SIM_PROC_DEAD) return;
    if (p->yielded != ev) return;     /* stale callback */
    schedule_resume(p, sim_event_value(ev));
}

/* Schedule the process for resumption at current time.
 *
 * The resume event is internal — the user never sees it — so it's
 * recycled in place across yields. If a resume is already pending we
 * coalesce (the second call wins on resume_value). */
static void schedule_resume(sim_process_t *p, void *value) {
    p->resume_value = value;
    sim_event_t *r = p->resume_ev;
    if (r && r->state == SIM_EV_TRIGGERED) {
        /* Already pending — value updated, nothing else to do. */
        return;
    }
    if (!r) {
        r = _sim_event_alloc(p->env);
        p->resume_ev = r;
    } else {
        /* Recycle in place: callbacks list is already empty. */
        r->state       = SIM_EV_PENDING;
        r->ok          = 1;
        r->value       = NULL;
        r->fail_reason = NULL;
    }
    r->state = SIM_EV_TRIGGERED;
    sim_event_on(r, resume_cb, p);
    _sim_event_schedule(r, 0.0, SIM_PRIO_NORMAL);
}

/* Bottom-of-stack entry. user-fn -> done event -> recycle -> host. */
static void process_entry(void *arg) {
    sim_process_t *p = (sim_process_t *)arg;
    p->state = SIM_PROC_RUNNING;
    p->fn(p, p->arg);
    /* Only succeed `done` if someone asked for it. Fire-and-forget
     * processes never allocate the event in the first place. */
    if (p->done && !sim_event_triggered(p->done)) {
        sim_event_succeed(p->done, NULL);
    }
    p->state = SIM_PROC_DEAD;
    /* Return the process to the env pool — preserves the stack and the
     * per-process resume event for the next sim_process(). */
    sim_env_t *env = p->env;
    p->free_next = env->process_pool;
    env->process_pool = p;
    coro_switch(&p->ctx, &env->host_ctx);
    /* Unreachable. */
}

sim_process_t *sim_process(sim_env_t *env, sim_proc_fn fn, void *arg) {
    sim_process_t *p;
    if (env->process_pool) {
        /* Reuse a dead process: keep its stack + resume_ev allocation,
         * reset the rest, re-init the coroutine context. */
        p = env->process_pool;
        env->process_pool = p->free_next;
        void          *stack_save     = p->stack_base;
        size_t         stack_size     = p->stack_size;
        sim_event_t   *resume_ev_save = p->resume_ev;
        sim_process_t *all_next_save  = p->all_next;
        memset(p, 0, sizeof(*p));
        p->stack_base = stack_save;
        p->stack_size = stack_size;
        p->resume_ev  = resume_ev_save;
        p->all_next   = all_next_save;
    } else {
        p = (sim_process_t *)calloc(1, sizeof(*p));
        if (!p) return NULL;
        p->stack_size = SIM_DEFAULT_STACK;
        p->stack_base = malloc(p->stack_size);
        if (!p->stack_base) { free(p); return NULL; }
        p->all_next = env->all_processes;
        env->all_processes = p;
    }

    p->env   = env;
    p->fn    = fn;
    p->arg   = arg;
    p->state = SIM_PROC_NEW;
    /* p->done stays NULL: allocated lazily on first sim_process_event(). */

    coro_init(&p->ctx, p->stack_base, p->stack_size,
              process_entry, p);

    schedule_resume(p, NULL);
    return p;
}

sim_event_t *sim_process_event(sim_process_t *p) {
    if (!p->done) {
        p->done = _sim_event_alloc(p->env);
        /* If the process already finished before anyone asked for its
         * event, fire it immediately so a yield resolves promptly. */
        if (p->state == SIM_PROC_DEAD) {
            sim_event_succeed(p->done, NULL);
        }
    }
    return p->done;
}

void *sim_yield(sim_process_t *self, sim_event_t *evt) {
    self->yielded = evt;
    if (sim_event_processed(evt)) {
        schedule_resume(self, sim_event_value(evt));
    } else {
        sim_event_on(evt, waiter_cb, self);
    }
    coro_switch(&self->ctx, &self->env->host_ctx);

    /* Resumed. Clear waiting state; surface interrupt if any. */
    self->yielded = NULL;
    if (self->interrupt_pending) {
        self->interrupt_pending = 0;
        self->was_interrupted   = 1;
        void *cause = self->interrupt_cause;
        self->interrupt_cause = NULL;
        return cause;
    }
    self->was_interrupted = 0;
    return self->resume_value;
}

void sim_process_interrupt(sim_process_t *p, void *cause) {
    if (!p || p->state == SIM_PROC_DEAD) return;
    if (p->interrupt_pending) return;          /* coalesce */
    p->interrupt_pending = 1;
    p->interrupt_cause   = cause;
    /* Cancel any pending wait; the old event's later fire will see
     * yielded != ev and skip. */
    p->yielded = NULL;
    schedule_resume(p, cause);
}

int   sim_process_was_interrupted(const sim_process_t *self) {
    return self ? (int)self->was_interrupted : 0;
}
void *sim_process_interrupt_cause(const sim_process_t *self) {
    return self ? self->interrupt_cause : NULL;
}
