/* Throughput benchmark: many processes each yielding many timeouts.
 *
 * Print: events processed, wall time, events/sec, ns/event. */

#include "simpyc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { sim_env_t *env; int n_yields; } bench_arg_t;

static long g_count;

static void runner(sim_process_t *self, void *arg) {
    bench_arg_t *a = (bench_arg_t *)arg;
    for (int i = 0; i < a->n_yields; i++) {
        sim_yield(self, sim_timeout(a->env, 1.0));
        g_count++;
    }
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int n_procs   = argc > 1 ? atoi(argv[1]) : 1000;
    int n_yields  = argc > 2 ? atoi(argv[2]) : 1000;

    sim_env_t *env = sim_env_create();
    bench_arg_t a = { env, n_yields };
    for (int i = 0; i < n_procs; i++) sim_process(env, runner, &a);

    double t0 = now_sec();
    size_t events = sim_run(env);
    double dt = now_sec() - t0;

    long total_yields = (long)n_procs * n_yields;
    printf("processes:        %d\n", n_procs);
    printf("yields/process:   %d\n", n_yields);
    printf("yield count:      %ld\n", total_yields);
    printf("queue events:     %zu\n", events);
    printf("wall:             %.3fs\n", dt);
    printf("events/sec:       %.2fM\n", events / dt / 1e6);
    printf("ns/event:         %.1f\n", dt * 1e9 / events);
    printf("ns/yield:         %.1f\n", dt * 1e9 / total_yields);

    sim_env_destroy(env);
    return 0;
}
