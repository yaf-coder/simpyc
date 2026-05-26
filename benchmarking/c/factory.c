/* Factory simulator using simpyc.
 *
 * Mirrors benchmarking/python/factory.py. Same parameters, same primitives,
 * same end-state. Exercises every public function in simpyc.h. */

#include "simpyc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef struct {
    int    num_workers;
    int    num_shippers;
    double sim_time;
    double material_capacity;
    double material_initial;
    double material_per_product;
    double assembly_time;
    double refill_interval;
    double refill_amount;
    double ship_time;
    int    production_target;
} params_t;

typedef struct {
    int    produced;
    int    shipped;
    double target_hit_at;
} stats_t;

typedef struct {
    sim_env_t       *env;
    sim_container_t *materials;
    sim_resource_t  *workers;
    sim_store_t     *store;
    sim_event_t     *target;
    params_t        *p;
    stats_t         *s;
} ctx_t;

static void on_target(sim_event_t *ev, void *user) {
    (void)ev;
    ctx_t *c = (ctx_t *)user;
    c->s->target_hit_at = sim_now(c->env);
}

static void refiller(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        sim_yield(self, sim_timeout(c->env, c->p->refill_interval));
        sim_yield(self, sim_container_put(c->materials, c->p->refill_amount));
    }
}

static void worker(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        sim_event_t *req = sim_resource_request(c->workers);
        sim_yield(self, req);
        sim_yield(self, sim_container_get(c->materials,
                                          c->p->material_per_product));
        sim_yield(self, sim_timeout(c->env, c->p->assembly_time));
        sim_yield(self, sim_store_put(c->store, c));
        sim_resource_release(c->workers, req);
        c->s->produced++;
        if (c->s->produced == c->p->production_target
            && !sim_event_triggered(c->target)) {
            sim_event_succeed(c->target, NULL);
        }
    }
}

static void shipper(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        sim_yield(self, sim_store_get(c->store));
        sim_yield(self, sim_timeout(c->env, c->p->ship_time));
        c->s->shipped++;
    }
}

static void watchdog(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    /* AnyOf: production target or a soft half-time deadline. */
    sim_event_t *deadline = sim_timeout(c->env, c->p->sim_time * 0.5);
    sim_event_t *waits[2] = { c->target, deadline };
    sim_yield(self, sim_any_of(c->env, waits, 2));
    /* AllOf: small bundle to exercise the constructor. */
    sim_event_t *t1 = sim_timeout(c->env, 1.0);
    sim_event_t *t2 = sim_timeout(c->env, 2.0);
    sim_event_t *all[2] = { t1, t2 };
    sim_yield(self, sim_all_of(c->env, all, 2));
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double peak_rss_mb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    /* macOS reports bytes. */
    return (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
    /* Linux reports KB. */
    return (double)ru.ru_maxrss / 1024.0;
#endif
}

/* Simple --key=value parser. Mirrors the Python argparse layout. */
static void parse_args(int argc, char **argv, params_t *p) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--", 2) != 0) continue;
        const char *eq = strchr(a, '=');
        if (!eq) continue;
        char key[64];
        size_t klen = (size_t)(eq - (a + 2));
        if (klen == 0 || klen >= sizeof(key)) continue;
        memcpy(key, a + 2, klen);
        key[klen] = '\0';
        const char *v = eq + 1;
        if      (!strcmp(key, "num-workers"))          p->num_workers = atoi(v);
        else if (!strcmp(key, "num-shippers"))         p->num_shippers = atoi(v);
        else if (!strcmp(key, "sim-time"))             p->sim_time = atof(v);
        else if (!strcmp(key, "material-capacity"))    p->material_capacity = atof(v);
        else if (!strcmp(key, "material-initial"))     p->material_initial = atof(v);
        else if (!strcmp(key, "material-per-product")) p->material_per_product = atof(v);
        else if (!strcmp(key, "assembly-time"))        p->assembly_time = atof(v);
        else if (!strcmp(key, "refill-interval"))      p->refill_interval = atof(v);
        else if (!strcmp(key, "refill-amount"))        p->refill_amount = atof(v);
        else if (!strcmp(key, "ship-time"))            p->ship_time = atof(v);
        else if (!strcmp(key, "production-target"))    p->production_target = atoi(v);
    }
}

int main(int argc, char **argv) {
    params_t p = {
        .num_workers          = 4,
        .num_shippers         = 2,
        .sim_time             = 10000.0,
        .material_capacity    = 100.0,
        .material_initial     = 50.0,
        .material_per_product = 2.0,
        .assembly_time        = 5.0,
        .refill_interval      = 8.0,
        .refill_amount        = 20.0,
        .ship_time            = 3.0,
        .production_target    = 500,
    };
    parse_args(argc, argv, &p);

    stats_t s = { .produced = 0, .shipped = 0, .target_hit_at = -1.0 };

    sim_env_t *env = sim_env_create();
    ctx_t ctx = {
        .env       = env,
        .materials = sim_container_create(env, p.material_capacity,
                                          p.material_initial),
        .workers   = sim_resource_create(env, p.num_workers),
        .store     = sim_store_create(env, 0),
        .target    = sim_event(env),
        .p = &p,
        .s = &s,
    };
    sim_event_on(ctx.target, on_target, &ctx);

    sim_process(env, refiller, &ctx);
    sim_process(env, watchdog, &ctx);
    for (int i = 0; i < p.num_workers;  i++) sim_process(env, worker,  &ctx);
    for (int i = 0; i < p.num_shippers; i++) sim_process(env, shipper, &ctx);

    double t0 = now_sec();
    sim_run_until(env, p.sim_time);
    double t1 = now_sec();

    printf("impl=simpyc\n");
    printf("sim_time=%.1f\n",        sim_now(env));
    printf("produced=%d\n",          s.produced);
    printf("shipped=%d\n",           s.shipped);
    printf("target_hit_at=%.2f\n",   s.target_hit_at);
    printf("wall_seconds=%.6f\n",    t1 - t0);
    printf("peak_rss_mb=%.2f\n",     peak_rss_mb());

    sim_container_destroy(ctx.materials);
    sim_resource_destroy(ctx.workers);
    sim_store_destroy(ctx.store);
    sim_env_destroy(env);
    return 0;
}
