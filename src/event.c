/* Events: pending → triggered → processed.
 * Allocation is pooled per-env to avoid malloc churn in tight loops. */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

sim_event_t *_sim_event_alloc(sim_env_t *env) {
    sim_event_t *e;
    if (env->event_pool) {
        e = env->event_pool;
        env->event_pool = e->free_next;
        sim_event_t *saved_all_next = e->all_next;
        memset(e, 0, sizeof(*e));
        e->all_next = saved_all_next;
    } else {
        e = (sim_event_t *)calloc(1, sizeof(*e));
        if (!e) return NULL;
        e->all_next = env->all_events;
        env->all_events = e;
    }
    e->env = env;
    e->state = SIM_EV_PENDING;
    e->ok = 1;
    return e;
}

sim_event_t *sim_event(sim_env_t *env) {
    return _sim_event_alloc(env);
}

void _sim_event_schedule(sim_event_t *e, double delay, int priority) {
    heap_entry_t he;
    he.time     = e->env->now + delay;
    he.priority = priority;
    he.seq      = e->env->next_seq++;
    he.event    = e;
    heap_push(&e->env->heap, &he);
}

sim_event_t *sim_timeout(sim_env_t *env, double delay) {
    return sim_timeout_v(env, delay, NULL);
}

sim_event_t *sim_timeout_v(sim_env_t *env, double delay, void *value) {
    sim_event_t *e = _sim_event_alloc(env);
    if (!e) return NULL;
    e->value = value;
    e->state = SIM_EV_TRIGGERED;
    /* Timeouts have no externally-visible identity beyond their value,
     * which sim_yield surfaces directly. Safe to recycle post-process. */
    e->recyclable = 1;
    _sim_event_schedule(e, delay, SIM_PRIO_NORMAL);
    return e;
}

void sim_event_succeed(sim_event_t *e, void *value) {
    if (e->state != SIM_EV_PENDING) return;
    e->value = value;
    e->ok    = 1;
    e->state = SIM_EV_TRIGGERED;
    _sim_event_schedule(e, 0.0, SIM_PRIO_NORMAL);
}

void sim_event_fail(sim_event_t *e, const char *reason) {
    if (e->state != SIM_EV_PENDING) return;
    e->fail_reason = reason;
    e->ok    = 0;
    e->state = SIM_EV_TRIGGERED;
    _sim_event_schedule(e, 0.0, SIM_PRIO_NORMAL);
}

int   sim_event_triggered(const sim_event_t *e) { return e->state >= SIM_EV_TRIGGERED; }
int   sim_event_processed(const sim_event_t *e) { return e->state == SIM_EV_PROCESSED; }
int   sim_event_ok       (const sim_event_t *e) { return e->ok; }
void *sim_event_value    (const sim_event_t *e) { return e->value; }
const char *sim_event_reason(const sim_event_t *e) { return e->fail_reason; }

void sim_event_on(sim_event_t *e, sim_callback_fn cb, void *user) {
    /* Already-processed events fire the callback immediately, matching
     * SimPy semantics for late subscribers. */
    if (e->state == SIM_EV_PROCESSED) {
        cb(e, user);
        return;
    }
    cb_node_t *n = (cb_node_t *)malloc(sizeof(*n));
    n->fn   = cb;
    n->user = user;
    n->next = NULL;
    if (e->callbacks_tail) e->callbacks_tail->next = n;
    else                   e->callbacks = n;
    e->callbacks_tail = n;
}

void _sim_event_run_callbacks(sim_event_t *e) {
    cb_node_t *n = e->callbacks;
    e->callbacks = e->callbacks_tail = NULL;
    e->state = SIM_EV_PROCESSED;
    while (n) {
        cb_node_t *next = n->next;
        n->fn(e, n->user);
        free(n);
        n = next;
    }
    /* Return recyclable events to the pool so long simulations don't
     * grow without bound. Caller must not touch e after this. */
    if (e->recyclable) {
        sim_env_t *env = e->env;
        e->free_next = env->event_pool;
        env->event_pool = e;
    }
}
