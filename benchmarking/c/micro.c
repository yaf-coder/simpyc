/* Per-primitive micro-benchmarks for simpyc. Mirrors python/micro.py.
 * Each bench prints `bench=<name> ops=<N> wall_seconds=<t> ns_per_op=<x>`. */

#include "simpyc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double peak_rss_mb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
    return (double)ru.ru_maxrss / 1024.0;
#endif
}

typedef struct { sim_env_t *env; int n; } na_t;

/* ----------------------------------------------------------- benches */

static void p_timeout(sim_process_t *self, void *arg) {
    na_t *a = (na_t *)arg;
    for (int i = 0; i < a->n; i++) sim_yield(self, sim_timeout(a->env, 1.0));
}
static void b_timeout(int n) {
    sim_env_t *e = sim_env_create();
    na_t a = { e, n };
    sim_process(e, p_timeout, &a);
    sim_run(e);
    sim_env_destroy(e);
}

static void b_event_succeed(int n) {
    sim_env_t *e = sim_env_create();
    for (int i = 0; i < n; i++) sim_event_succeed(sim_event(e), NULL);
    sim_run(e);
    sim_env_destroy(e);
}

static void noop_cb(sim_event_t *ev, void *user) { (void)ev; (void)user; }
static void b_callback(int n) {
    sim_env_t *e = sim_env_create();
    sim_event_t *ev = sim_event(e);
    for (int i = 0; i < n; i++) sim_event_on(ev, noop_cb, NULL);
    sim_event_succeed(ev, NULL);
    sim_run(e);
    sim_env_destroy(e);
}

static void p_noop(sim_process_t *self, void *arg) { (void)self; (void)arg; }
static void b_process_spawn(int n) {
    sim_env_t *e = sim_env_create();
    for (int i = 0; i < n; i++) sim_process(e, p_noop, NULL);
    sim_run(e);
    sim_env_destroy(e);
}

/* process-churn: spawn-yield-die-respawn pattern. Demonstrates the
 * process pool — after warm-up no malloc(stack) per spawn. */
typedef struct { sim_env_t *env; int n; } churn_arg_t;
static void churn_spawner(sim_process_t *self, void *arg) {
    churn_arg_t *a = (churn_arg_t *)arg;
    for (int i = 0; i < a->n; i++) {
        sim_process_t *p = sim_process(a->env, p_noop, NULL);
        sim_yield(self, sim_process_event(p));  /* wait for child to die */
    }
}
static void b_process_churn(int n) {
    sim_env_t *e = sim_env_create();
    churn_arg_t a = { e, n };
    sim_process(e, churn_spawner, &a);
    sim_run(e);
    sim_env_destroy(e);
}

typedef struct { sim_env_t *env; int n; sim_resource_t *r; } na_res_t;
static void p_resource(sim_process_t *self, void *arg) {
    na_res_t *a = (na_res_t *)arg;
    for (int i = 0; i < a->n; i++) {
        sim_event_t *req = sim_resource_request(a->r);
        sim_yield(self, req);
        sim_resource_release(a->r, req);
    }
}
static void b_resource(int n) {
    sim_env_t *e = sim_env_create();
    sim_resource_t *r = sim_resource_create(e, 1);
    na_res_t a = { e, n, r };
    sim_process(e, p_resource, &a);
    sim_run(e);
    sim_resource_destroy(r);
    sim_env_destroy(e);
}

typedef struct { sim_env_t *env; int n; sim_priority_resource_t *r; } na_pres_t;
static void p_pres(sim_process_t *self, void *arg) {
    na_pres_t *a = (na_pres_t *)arg;
    for (int i = 0; i < a->n; i++) {
        sim_event_t *req = sim_priority_resource_request(a->r, 0);
        sim_yield(self, req);
        sim_priority_resource_release(a->r, req);
    }
}
static void b_priority_resource(int n) {
    sim_env_t *e = sim_env_create();
    sim_priority_resource_t *r = sim_priority_resource_create(e, 1);
    na_pres_t a = { e, n, r };
    sim_process(e, p_pres, &a);
    sim_run(e);
    sim_priority_resource_destroy(r);
    sim_env_destroy(e);
}

typedef struct { sim_env_t *env; int n; sim_preemptive_resource_t *r; } na_pre_t;
static void p_pre(sim_process_t *self, void *arg) {
    na_pre_t *a = (na_pre_t *)arg;
    for (int i = 0; i < a->n; i++) {
        sim_event_t *req = sim_preemptive_resource_request(a->r, self, 0, 0);
        sim_yield(self, req);
        sim_preemptive_resource_release(a->r, req);
    }
}
static void b_preemptive_resource(int n) {
    sim_env_t *e = sim_env_create();
    sim_preemptive_resource_t *r = sim_preemptive_resource_create(e, 1);
    na_pre_t a = { e, n, r };
    sim_process(e, p_pre, &a);
    sim_run(e);
    sim_preemptive_resource_destroy(r);
    sim_env_destroy(e);
}

typedef struct { sim_env_t *env; int n; sim_container_t *c; } na_c_t;
static void p_cprod(sim_process_t *self, void *arg) {
    na_c_t *a = (na_c_t *)arg;
    for (int i = 0; i < a->n; i++) sim_yield(self, sim_container_put(a->c, 1.0));
}
static void p_ccons(sim_process_t *self, void *arg) {
    na_c_t *a = (na_c_t *)arg;
    for (int i = 0; i < a->n; i++) sim_yield(self, sim_container_get(a->c, 1.0));
}
static void b_container(int n) {
    sim_env_t *e = sim_env_create();
    sim_container_t *c = sim_container_create(e, (double)n * 2, 0.0);
    na_c_t a = { e, n, c };
    sim_process(e, p_cprod, &a);
    sim_process(e, p_ccons, &a);
    sim_run(e);
    sim_container_destroy(c);
    sim_env_destroy(e);
}

typedef struct { sim_env_t *env; int n; sim_store_t *s; } na_s_t;
static void p_sprod(sim_process_t *self, void *arg) {
    na_s_t *a = (na_s_t *)arg;
    for (intptr_t i = 0; i < a->n; i++)
        sim_yield(self, sim_store_put(a->s, (void *)i));
}
static void p_scons(sim_process_t *self, void *arg) {
    na_s_t *a = (na_s_t *)arg;
    for (int i = 0; i < a->n; i++) sim_yield(self, sim_store_get(a->s));
}
static void b_store(int n) {
    sim_env_t *e = sim_env_create();
    sim_store_t *s = sim_store_create(e, 0);
    na_s_t a = { e, n, s };
    sim_process(e, p_sprod, &a);
    sim_process(e, p_scons, &a);
    sim_run(e);
    sim_store_destroy(s);
    sim_env_destroy(e);
}

static int accept_all(void *item, void *user) { (void)item; (void)user; return 1; }
typedef struct { sim_env_t *env; int n; sim_filter_store_t *s; } na_fs_t;
static void p_fsprod(sim_process_t *self, void *arg) {
    na_fs_t *a = (na_fs_t *)arg;
    for (intptr_t i = 0; i < a->n; i++)
        sim_yield(self, sim_filter_store_put(a->s, (void *)i));
}
static void p_fscons(sim_process_t *self, void *arg) {
    na_fs_t *a = (na_fs_t *)arg;
    for (int i = 0; i < a->n; i++)
        sim_yield(self, sim_filter_store_get(a->s, accept_all, NULL));
}
static void b_filter_store(int n) {
    sim_env_t *e = sim_env_create();
    sim_filter_store_t *s = sim_filter_store_create(e, 0);
    na_fs_t a = { e, n, s };
    sim_process(e, p_fsprod, &a);
    sim_process(e, p_fscons, &a);
    sim_run(e);
    sim_filter_store_destroy(s);
    sim_env_destroy(e);
}

typedef struct { sim_env_t *env; int n; sim_priority_store_t *s; } na_ps_t;
static void p_psprod(sim_process_t *self, void *arg) {
    na_ps_t *a = (na_ps_t *)arg;
    for (intptr_t i = 0; i < a->n; i++)
        sim_yield(self, sim_priority_store_put(a->s, (int)i, (void *)i));
}
static void p_pscons(sim_process_t *self, void *arg) {
    na_ps_t *a = (na_ps_t *)arg;
    for (int i = 0; i < a->n; i++) sim_yield(self, sim_priority_store_get(a->s));
}
static void b_priority_store(int n) {
    sim_env_t *e = sim_env_create();
    sim_priority_store_t *s = sim_priority_store_create(e, 0);
    na_ps_t a = { e, n, s };
    sim_process(e, p_psprod, &a);
    sim_process(e, p_pscons, &a);
    sim_run(e);
    sim_priority_store_destroy(s);
    sim_env_destroy(e);
}

static void p_allof(sim_process_t *self, void *arg) {
    na_t *a = (na_t *)arg;
    for (int i = 0; i < a->n; i++) {
        sim_event_t *evs[4] = {
            sim_timeout(a->env, 0), sim_timeout(a->env, 0),
            sim_timeout(a->env, 0), sim_timeout(a->env, 0),
        };
        sim_yield(self, sim_all_of(a->env, evs, 4));
    }
}
static void b_allof(int n) {
    sim_env_t *e = sim_env_create();
    na_t a = { e, n };
    sim_process(e, p_allof, &a);
    sim_run(e);
    sim_env_destroy(e);
}

static void p_anyof(sim_process_t *self, void *arg) {
    na_t *a = (na_t *)arg;
    for (int i = 0; i < a->n; i++) {
        sim_event_t *evs[4] = {
            sim_timeout(a->env, 0), sim_timeout(a->env, 0),
            sim_timeout(a->env, 0), sim_timeout(a->env, 0),
        };
        sim_yield(self, sim_any_of(a->env, evs, 4));
    }
}
static void b_anyof(int n) {
    sim_env_t *e = sim_env_create();
    na_t a = { e, n };
    sim_process(e, p_anyof, &a);
    sim_run(e);
    sim_env_destroy(e);
}

typedef struct { sim_env_t *env; int n; sim_process_t *victim; } na_int_t;
static void p_victim(sim_process_t *self, void *arg) {
    na_int_t *a = (na_int_t *)arg;
    int cnt = 0;
    while (cnt < a->n) {
        sim_yield(self, sim_timeout(a->env, 1e9));
        if (sim_process_was_interrupted(self)) cnt++;
    }
}
static void p_driver(sim_process_t *self, void *arg) {
    na_int_t *a = (na_int_t *)arg;
    for (int i = 0; i < a->n; i++) {
        sim_yield(self, sim_timeout(a->env, 0));
        sim_process_interrupt(a->victim, (void *)(intptr_t)1);
    }
}
static void b_interrupt(int n) {
    sim_env_t *e = sim_env_create();
    na_int_t a = { e, n, NULL };
    a.victim = sim_process(e, p_victim, &a);
    sim_process(e, p_driver, &a);
    sim_run(e);
    sim_env_destroy(e);
}

/* ----------------------------------------------------------- runner */

typedef void (*bench_fn)(int);
typedef struct { const char *name; bench_fn fn; double n_scale; } bench_t;

static const bench_t BENCHES[] = {
    {"timeout",             b_timeout,             1.0},
    {"event-succeed",       b_event_succeed,       1.0},
    {"callback",            b_callback,            1.0},
    {"process-spawn",       b_process_spawn,       0.1},
    {"process-churn",       b_process_churn,       1.0},
    {"resource",            b_resource,            1.0},
    {"priority-resource",   b_priority_resource,   1.0},
    {"preemptive-resource", b_preemptive_resource, 1.0},
    {"container",           b_container,           1.0},
    {"store",               b_store,               1.0},
    {"filter-store",        b_filter_store,        1.0},
    {"priority-store",      b_priority_store,      1.0},
    {"allof",               b_allof,               1.0},
    {"anyof",               b_anyof,               1.0},
    {"interrupt",           b_interrupt,           1.0},
};
#define NBENCHES (sizeof(BENCHES) / sizeof(BENCHES[0]))

static int name_in_set(const char *name, const char *csv) {
    if (!csv) return 1;
    size_t nl = strlen(name);
    const char *p = csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == nl && memcmp(p, name, nl) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int n = 100000;
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--only=", 7)) only = argv[i] + 7;
    }

    printf("impl=simpyc n=%d\n", n);
    fflush(stdout);
    for (size_t i = 0; i < NBENCHES; i++) {
        if (!name_in_set(BENCHES[i].name, only)) continue;
        int n_use = (int)((double)n * BENCHES[i].n_scale);
        if (n_use < 1000) n_use = 1000;
        double t0 = now_sec();
        BENCHES[i].fn(n_use);
        double dt = now_sec() - t0;
        printf("bench=%s ops=%d wall_seconds=%.6f ns_per_op=%.1f\n",
               BENCHES[i].name, n_use, dt, dt * 1e9 / n_use);
        fflush(stdout);
    }
    printf("peak_rss_mb=%.2f\n", peak_rss_mb());
    return 0;
}
