/* Processes: each runs on its own stack via the coro library.
 *
 * Lifecycle:
 *   sim_process()      -> alloc + coro_init; immediately scheduled to run.
 *   first dispatch     -> trampoline runs user fn until it yields.
 *   sim_yield(self,e)  -> subscribe callback resume_cb to e; switch out.
 *   resume_cb fires    -> coro_switch back into the process from host.
 *   user fn returns    -> succeed `done`; switch out permanently.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define SIM_DEFAULT_STACK   (64 * 1024)   /* 64 KiB; tune per-workload */

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
 * with the event's value. */
static void waiter_cb(sim_event_t *ev, void *user) {
    sim_process_t *p = (sim_process_t *)user;
    if (p->state == SIM_PROC_DEAD) return;
    schedule_resume(p, sim_event_value(ev));
}

/* Schedule the process for resumption at current time, normal priority. */
static void schedule_resume(sim_process_t *p, void *value) {
    p->resume_value = value;
    sim_event_t *r = _sim_event_alloc(p->env);
    r->state = SIM_EV_TRIGGERED;
    /* Add the resume callback before scheduling (callback list isn't
     * scanned until run-time). */
    sim_event_on(r, resume_cb, p);
    _sim_event_schedule(r, 0.0, SIM_PRIO_NORMAL);
}

/* Bottom-of-stack entry. user-fn -> done event -> host. */
static void process_entry(void *arg) {
    sim_process_t *p = (sim_process_t *)arg;
    p->state = SIM_PROC_RUNNING;
    p->fn(p, p->arg);
    /* Body returned: trigger done event and switch back permanently. */
    if (!sim_event_triggered(p->done)) {
        sim_event_succeed(p->done, NULL);
    }
    p->state = SIM_PROC_DEAD;
    coro_switch(&p->ctx, &p->env->host_ctx);
    /* Unreachable; the host won't switch back in. */
}

sim_process_t *sim_process(sim_env_t *env, sim_proc_fn fn, void *arg) {
    sim_process_t *p = (sim_process_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->env  = env;
    p->fn   = fn;
    p->arg  = arg;
    p->state = SIM_PROC_NEW;
    p->stack_size = SIM_DEFAULT_STACK;
    p->stack_base = malloc(p->stack_size);
    if (!p->stack_base) { free(p); return NULL; }

    p->done = _sim_event_alloc(env);

    coro_init(&p->ctx, p->stack_base, p->stack_size,
              process_entry, p);

    /* Link into the env's all-list for destroy. */
    p->all_next = env->all_processes;
    env->all_processes = p;

    /* Kick: schedule first run at now. */
    schedule_resume(p, NULL);
    return p;
}

sim_event_t *sim_process_event(sim_process_t *p) { return p->done; }

void *sim_yield(sim_process_t *self, sim_event_t *evt) {
    self->yielded = evt;
    if (sim_event_processed(evt)) {
        /* Already done — schedule self-resume directly with the value. */
        schedule_resume(self, sim_event_value(evt));
    } else {
        /* Subscribe; waiter_cb will schedule us when evt fires. */
        sim_event_on(evt, waiter_cb, self);
    }
    coro_switch(&self->ctx, &self->env->host_ctx);
    return self->resume_value;
}
