/* Environment + scheduler loop. */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

sim_env_t *sim_env_create(void) {
    sim_env_t *env = (sim_env_t *)calloc(1, sizeof(*env));
    if (!env) return NULL;
    heap_init(&env->heap);
    return env;
}

void sim_env_destroy(sim_env_t *env) {
    if (!env) return;

    /* Kill any live processes — free their stacks. We don't run their
     * code; the simulation is being torn down. */
    sim_process_t *p = env->all_processes;
    while (p) {
        sim_process_t *next = p->all_next;
        free(p->stack_base);
        free(p);
        p = next;
    }

    /* Free all events. callbacks lists were freed at processing time;
     * any pending ones we leak intentionally rather than chase. Free
     * the cb_node_t chains here for any still-pending events. */
    sim_event_t *e = env->all_events;
    while (e) {
        sim_event_t *next = e->all_next;
        cb_node_t *n = e->callbacks;
        while (n) { cb_node_t *nn = n->next; free(n); n = nn; }
        free(e);
        e = next;
    }

    heap_free(&env->heap);
    free(env);
}

double sim_now(const sim_env_t *env) { return env->now; }

int sim_peek(const sim_env_t *env, double *out_time) {
    heap_entry_t e;
    if (heap_peek(&env->heap, &e) < 0) return -1;
    if (out_time) *out_time = e.time;
    return 0;
}

int sim_step(sim_env_t *env) {
    heap_entry_t he;
    if (heap_pop(&env->heap, &he) < 0) return -1;
    env->now = he.time;
    _sim_event_run_callbacks(he.event);
    return 0;
}

size_t sim_run(sim_env_t *env) {
    size_t n = 0;
    while (sim_step(env) == 0) n++;
    return n;
}

size_t sim_run_until(sim_env_t *env, double until) {
    size_t n = 0;
    heap_entry_t he;
    while (heap_peek(&env->heap, &he) == 0 && he.time <= until) {
        if (sim_step(env) < 0) break;
        n++;
    }
    if (env->now < until) env->now = until;
    return n;
}
