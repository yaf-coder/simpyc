/* Factory simulator using simpyc.
 *
 * Mirrors benchmarking/python/factory.py line-for-line. Exercises every
 * primitive in simpyc.h:
 *
 *   Environment / Timeout / Event / Process
 *   AllOf / AnyOf
 *   Resource              -- stations
 *   PriorityResource      -- inspectors (rush items get priority)
 *   PreemptiveResource    -- machine; maintenance preempts workers
 *   Container             -- raw materials
 *   Store                 -- finished buffer
 *   FilterStore           -- parts bin (workers pull matching kinds)
 *   PriorityStore         -- shipping queue
 *   Process.interrupt     -- supervisor reviews reviewers
 */

#include "simpyc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef struct {
    int    num_workers;
    int    num_inspectors;
    int    num_reviewers;
    int    num_stations;
    int    num_part_kinds;
    double sim_time;
    double material_capacity;
    double material_initial;
    double material_per_product;
    double parts_interval;
    double assembly_time;
    double inspect_time;
    double refill_interval;
    double refill_amount;
    double ship_time;
    double maint_interval;
    double maint_time;
    double review_interval;
    double review_work;
    int    rush_every;
    int    production_target;
} params_t;

typedef struct {
    int    produced;
    int    shipped;
    int    preempted;
    int    reviewed;
    double target_hit_at;
} stats_t;

typedef struct { int kind; int id; }     part_t;
typedef struct { int id; int priority; } product_t;

typedef struct {
    sim_env_t                 *env;
    sim_container_t           *materials;
    sim_filter_store_t        *parts;
    sim_resource_t            *stations;
    sim_preemptive_resource_t *machine;
    sim_priority_resource_t   *inspectors;
    sim_priority_store_t      *ship_q;
    sim_store_t               *finished;
    sim_event_t               *target;
    sim_process_t            **reviewers;
    params_t                  *p;
    stats_t                   *s;
} ctx_t;

typedef struct { ctx_t *c; int idx; } worker_arg_t;
typedef struct { ctx_t *c; int idx; } reviewer_arg_t;

static void on_target(sim_event_t *ev, void *user) {
    (void)ev;
    ctx_t *c = (ctx_t *)user;
    c->s->target_hit_at = sim_now(c->env);
}

static int kind_filter(void *item, void *user) {
    return ((part_t *)item)->kind == (int)(intptr_t)user;
}

static void refiller(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        sim_yield(self, sim_timeout(c->env, c->p->refill_interval));
        sim_yield(self, sim_container_put(c->materials, c->p->refill_amount));
    }
}

static void parts_supplier(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    int n = 0;
    for (;;) {
        sim_yield(self, sim_timeout(c->env, c->p->parts_interval));
        part_t *part = (part_t *)malloc(sizeof(*part));
        part->kind = n % c->p->num_part_kinds;
        part->id   = n;
        n++;
        sim_yield(self, sim_filter_store_put(c->parts, part));
    }
}

static void maintenance(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        sim_yield(self, sim_timeout(c->env, c->p->maint_interval));
        sim_event_t *req = sim_preemptive_resource_request(
            c->machine, self, /*priority=*/0, /*preempt=*/1);
        sim_yield(self, req);
        sim_yield(self, sim_timeout(c->env, c->p->maint_time));
        sim_preemptive_resource_release(c->machine, req);
    }
}

static void worker(sim_process_t *self, void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    ctx_t *c = wa->c;
    int my_kind = wa->idx % c->p->num_part_kinds;
    for (;;) {
        sim_yield(self,
            sim_container_get(c->materials, c->p->material_per_product));
        part_t *part = (part_t *)sim_yield(self,
            sim_filter_store_get(c->parts, kind_filter,
                                 (void *)(intptr_t)my_kind));
        free(part);

        sim_event_t *sreq = sim_resource_request(c->stations);
        sim_yield(self, sreq);

        sim_event_t *mreq = sim_preemptive_resource_request(
            c->machine, self, /*priority=*/10, /*preempt=*/1);
        sim_yield(self, mreq);

        sim_yield(self, sim_timeout(c->env, c->p->assembly_time));
        int preempted = sim_process_was_interrupted(self);
        if (preempted) c->s->preempted++;
        sim_preemptive_resource_release(c->machine, mreq);

        if (!preempted) {
            int rush = (c->s->produced % c->p->rush_every == 0);
            int prio = rush ? 0 : 5;
            sim_event_t *ireq = sim_priority_resource_request(
                c->inspectors, prio);
            sim_yield(self, ireq);
            sim_yield(self, sim_timeout(c->env, c->p->inspect_time));
            sim_priority_resource_release(c->inspectors, ireq);

            product_t *prod = (product_t *)malloc(sizeof(*prod));
            prod->id       = c->s->produced;
            prod->priority = prio;
            sim_yield(self, sim_store_put(c->finished, prod));

            c->s->produced++;
            if (c->s->produced == c->p->production_target
                && !sim_event_triggered(c->target)) {
                sim_event_succeed(c->target, NULL);
            }
        }

        sim_resource_release(c->stations, sreq);
    }
}

static void aggregator(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        product_t *prod = (product_t *)sim_yield(self,
            sim_store_get(c->finished));
        sim_yield(self,
            sim_priority_store_put(c->ship_q, prod->priority, prod));
    }
}

static void dispatcher(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        product_t *prod = (product_t *)sim_yield(self,
            sim_priority_store_get(c->ship_q));
        sim_yield(self, sim_timeout(c->env, c->p->ship_time));
        free(prod);
        c->s->shipped++;
    }
}

static void reviewer(sim_process_t *self, void *arg) {
    reviewer_arg_t *ra = (reviewer_arg_t *)arg;
    ctx_t *c = ra->c;
    for (;;) {
        sim_yield(self, sim_timeout(c->env, c->p->review_work));
        if (sim_process_was_interrupted(self)) c->s->reviewed++;
    }
}

static void supervisor(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    for (;;) {
        sim_yield(self, sim_timeout(c->env, c->p->review_interval));
        for (int i = 0; i < c->p->num_reviewers; i++) {
            sim_process_interrupt(c->reviewers[i], (void *)(intptr_t)1);
        }
    }
}

static void watchdog(sim_process_t *self, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    sim_event_t *deadline = sim_timeout(c->env, c->p->sim_time * 0.5);
    sim_event_t *waits[2] = { c->target, deadline };
    sim_yield(self, sim_any_of(c->env, waits, 2));
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
    return (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
    return (double)ru.ru_maxrss / 1024.0;
#endif
}

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
#define INT_OPT(K, F) else if (!strcmp(key, K)) p->F = atoi(v)
#define DBL_OPT(K, F) else if (!strcmp(key, K)) p->F = atof(v)
        if (0) { }
        INT_OPT("num-workers",          num_workers);
        INT_OPT("num-inspectors",       num_inspectors);
        INT_OPT("num-reviewers",        num_reviewers);
        INT_OPT("num-stations",         num_stations);
        INT_OPT("num-part-kinds",       num_part_kinds);
        DBL_OPT("sim-time",             sim_time);
        DBL_OPT("material-capacity",    material_capacity);
        DBL_OPT("material-initial",     material_initial);
        DBL_OPT("material-per-product", material_per_product);
        DBL_OPT("parts-interval",       parts_interval);
        DBL_OPT("assembly-time",        assembly_time);
        DBL_OPT("inspect-time",         inspect_time);
        DBL_OPT("refill-interval",      refill_interval);
        DBL_OPT("refill-amount",        refill_amount);
        DBL_OPT("ship-time",            ship_time);
        DBL_OPT("maint-interval",       maint_interval);
        DBL_OPT("maint-time",           maint_time);
        DBL_OPT("review-interval",      review_interval);
        DBL_OPT("review-work",          review_work);
        INT_OPT("rush-every",           rush_every);
        INT_OPT("production-target",    production_target);
#undef INT_OPT
#undef DBL_OPT
    }
}

int main(int argc, char **argv) {
    params_t p = {
        .num_workers          = 4,
        .num_inspectors       = 2,
        .num_reviewers        = 2,
        .num_stations         = 4,
        .num_part_kinds       = 2,
        .sim_time             = 10000.0,
        .material_capacity    = 200.0,
        .material_initial     = 100.0,
        .material_per_product = 2.0,
        .parts_interval       = 2.0,
        .assembly_time        = 5.0,
        .inspect_time         = 2.0,
        .refill_interval      = 8.0,
        .refill_amount        = 20.0,
        .ship_time            = 3.0,
        .maint_interval       = 50.0,
        .maint_time           = 4.0,
        .review_interval      = 30.0,
        .review_work          = 20.0,
        .rush_every           = 5,
        .production_target    = 500,
    };
    parse_args(argc, argv, &p);

    stats_t s = { 0, 0, 0, 0, -1.0 };

    sim_env_t *env = sim_env_create();
    ctx_t ctx = {
        .env        = env,
        .materials  = sim_container_create(env, p.material_capacity,
                                           p.material_initial),
        .parts      = sim_filter_store_create(env, 0),
        .stations   = sim_resource_create(env, p.num_stations),
        .machine    = sim_preemptive_resource_create(env, 1),
        .inspectors = sim_priority_resource_create(env, p.num_inspectors),
        .ship_q     = sim_priority_store_create(env, 0),
        .finished   = sim_store_create(env, 0),
        .target     = sim_event(env),
        .p          = &p,
        .s          = &s,
    };
    sim_event_on(ctx.target, on_target, &ctx);

    /* Reviewers must be created before the supervisor so it has handles
     * to interrupt. */
    ctx.reviewers = (sim_process_t **)calloc(p.num_reviewers, sizeof(*ctx.reviewers));
    reviewer_arg_t *rargs = (reviewer_arg_t *)calloc(p.num_reviewers, sizeof(*rargs));
    for (int i = 0; i < p.num_reviewers; i++) {
        rargs[i].c = &ctx; rargs[i].idx = i;
        ctx.reviewers[i] = sim_process(env, reviewer, &rargs[i]);
    }

    sim_process(env, refiller,       &ctx);
    sim_process(env, parts_supplier, &ctx);
    sim_process(env, maintenance,    &ctx);
    sim_process(env, supervisor,     &ctx);
    sim_process(env, aggregator,     &ctx);
    sim_process(env, dispatcher,     &ctx);
    sim_process(env, watchdog,       &ctx);

    worker_arg_t *wargs = (worker_arg_t *)calloc(p.num_workers, sizeof(*wargs));
    for (int i = 0; i < p.num_workers; i++) {
        wargs[i].c = &ctx; wargs[i].idx = i;
        sim_process(env, worker, &wargs[i]);
    }

    double t0 = now_sec();
    sim_run_until(env, p.sim_time);
    double t1 = now_sec();

    printf("impl=simpyc\n");
    printf("sim_time=%.1f\n",      sim_now(env));
    printf("produced=%d\n",        s.produced);
    printf("shipped=%d\n",         s.shipped);
    printf("preempted=%d\n",       s.preempted);
    printf("reviewed=%d\n",        s.reviewed);
    printf("target_hit_at=%.2f\n", s.target_hit_at);
    printf("wall_seconds=%.6f\n",  t1 - t0);
    printf("peak_rss_mb=%.2f\n",   peak_rss_mb());

    sim_container_destroy(ctx.materials);
    sim_filter_store_destroy(ctx.parts);
    sim_resource_destroy(ctx.stations);
    sim_preemptive_resource_destroy(ctx.machine);
    sim_priority_resource_destroy(ctx.inspectors);
    sim_priority_store_destroy(ctx.ship_q);
    sim_store_destroy(ctx.finished);
    free(ctx.reviewers);
    free(rargs);
    free(wargs);
    sim_env_destroy(env);
    return 0;
}
