/* Events: pending → triggered → processed.
 *
 * Callbacks live in a packed dynamic array per event. Subscribing is a
 * single store (amortized — geometric realloc on growth); firing is a
 * tight loop over contiguous memory. Recyclable events are returned to
 * an env pool on processing; we keep the cb array allocation across
 * recycle so steady-state subscription costs no allocator work. */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

sim_event_t *_sim_event_alloc(sim_env_t *env) {
    sim_event_t *e;
    if (env->event_pool) {
        e = env->event_pool;
        env->event_pool = e->free_next;
        /* Preserve the cb array allocation and the all-events linkage
         * across recycle; reset everything else. */
        cb_pair_t *cbs_save     = e->cbs;
        uint32_t   cap_save     = e->cb_cap;
        sim_event_t *all_save   = e->all_next;
        memset(e, 0, sizeof(*e));
        e->cbs      = cbs_save;
        e->cb_cap   = cap_save;
        e->all_next = all_save;
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

static void cb_grow(sim_event_t *e) {
    uint32_t ncap = e->cb_cap ? e->cb_cap * 2 : 2;
    e->cbs = (cb_pair_t *)realloc(e->cbs, (size_t)ncap * sizeof(*e->cbs));
    e->cb_cap = ncap;
}

void sim_event_on(sim_event_t *e, sim_callback_fn cb, void *user) {
    /* Already-processed events fire the callback immediately, matching
     * SimPy semantics for late subscribers. */
    if (e->state == SIM_EV_PROCESSED) {
        cb(e, user);
        return;
    }
    if (e->cb_len == e->cb_cap) cb_grow(e);
    e->cbs[e->cb_len].fn   = cb;
    e->cbs[e->cb_len].user = user;
    e->cb_len++;
}

void _sim_event_run_callbacks(sim_event_t *e) {
    e->state = SIM_EV_PROCESSED;
    uint32_t len = e->cb_len;
    e->cb_len = 0;                  /* reset before dispatching */
    cb_pair_t *cbs = e->cbs;
    for (uint32_t i = 0; i < len; i++) cbs[i].fn(e, cbs[i].user);
    /* Recyclable events go back to the pool so long simulations don't
     * grow without bound. Caller must not touch e after this. */
    if (e->recyclable) {
        sim_env_t *env = e->env;
        e->free_next = env->event_pool;
        env->event_pool = e;
    }
}
