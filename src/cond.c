/* AllOf / AnyOf condition events. */

#include "internal.h"

#include <stdlib.h>

typedef struct cond_state {
    sim_event_t *out;
    int          n_total;
    int          n_fired;
    int          any;          /* 1 = AnyOf, 0 = AllOf */
} cond_state_t;

static void cond_cb(sim_event_t *ev, void *user) {
    (void)ev;
    cond_state_t *s = (cond_state_t *)user;
    if (sim_event_triggered(s->out)) return;     /* already resolved */
    s->n_fired++;
    int done = s->any ? (s->n_fired >= 1)
                      : (s->n_fired >= s->n_total);
    if (done) {
        sim_event_succeed(s->out, NULL);
        /* state is leaked-with-env; cleanup happens at env_destroy. */
    }
}

static sim_event_t *make_cond(sim_env_t *env, sim_event_t * const *evs,
                              size_t n, int any) {
    sim_event_t *out = _sim_event_alloc(env);
    cond_state_t *s = (cond_state_t *)calloc(1, sizeof(*s));
    s->out = out;
    s->n_total = (int)n;
    s->any = any;

    /* Pre-count already-processed events so we resolve immediately
     * if the threshold is met. */
    for (size_t i = 0; i < n; i++) {
        if (sim_event_processed(evs[i])) s->n_fired++;
    }
    int done = any ? (s->n_fired >= 1)
                   : (s->n_fired >= s->n_total);
    if (done) {
        sim_event_succeed(out, NULL);
        return out;
    }
    /* Subscribe to the rest. Already-processed events fire cb
     * immediately inside sim_event_on, which we don't want — guard
     * the count by skipping them here. */
    for (size_t i = 0; i < n; i++) {
        if (!sim_event_processed(evs[i])) {
            sim_event_on(evs[i], cond_cb, s);
        }
    }
    return out;
}

sim_event_t *sim_all_of(sim_env_t *env, sim_event_t * const *events, size_t n) {
    return make_cond(env, events, n, 0);
}

sim_event_t *sim_any_of(sim_env_t *env, sim_event_t * const *events, size_t n) {
    return make_cond(env, events, n, 1);
}
