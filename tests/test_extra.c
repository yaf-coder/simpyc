/* Smoke tests for the second wave: process interrupt, PriorityResource,
 * PreemptiveResource, FilterStore, PriorityStore, RealtimeEnvironment. */

#include "simpyc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond) do {                                  \
    if (!(cond)) {                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n",               \
                __FILE__, __LINE__, #cond);               \
        exit(1);                                          \
    }                                                     \
} while (0)

static double wall_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- 1. Process interrupt fires while waiting on a timeout ---- */
typedef struct {
    sim_env_t     *env;
    sim_process_t *victim;
    int           *flag;
    double        *interrupted_at;
    double        *finished_at;
    void          *cause;
} intr_arg_t;

static void victim_proc(sim_process_t *self, void *arg) {
    intr_arg_t *a = (intr_arg_t *)arg;
    /* Wait far in the future; we expect to be interrupted before then. */
    void *r = sim_yield(self, sim_timeout(a->env, 1000.0));
    CHECK(sim_process_was_interrupted(self));
    CHECK(r == a->cause);
    *a->interrupted_at = sim_now(a->env);
    /* Resume normally after handling the interrupt. */
    sim_yield(self, sim_timeout(a->env, 5.0));
    CHECK(!sim_process_was_interrupted(self));
    *a->flag = 1;
    *a->finished_at = sim_now(a->env);
}

static void interruptor_proc(sim_process_t *self, void *arg) {
    intr_arg_t *a = (intr_arg_t *)arg;
    sim_yield(self, sim_timeout(a->env, 10.0));
    sim_process_interrupt(a->victim, a->cause);
}

static void test_interrupt(void) {
    sim_env_t *env = sim_env_create();
    int flag = 0;
    double interrupted_at = -1, finished_at = -1;
    intr_arg_t a = { env, NULL, &flag, &interrupted_at, &finished_at,
                     (void *)0xCAFEBABEUL };
    a.victim = sim_process(env, victim_proc, &a);
    sim_process(env, interruptor_proc, &a);
    sim_run(env);
    CHECK(flag == 1);
    CHECK(interrupted_at == 10.0);
    CHECK(finished_at == 15.0);
    /* Note: sim_now ends up at 1000 because the original (now-stale)
     * timeout still pops from the heap; its callback is a no-op. */
    sim_env_destroy(env);
}

/* ---- 2. PriorityResource: higher priority is served first ---- */
typedef struct {
    sim_env_t              *env;
    sim_priority_resource_t *r;
    int                    *order;
    int                    *idx;
    int                     id;
    int                     prio;
    double                  delay;
} pr_arg_t;

static void pr_worker(sim_process_t *self, void *arg) {
    pr_arg_t *a = (pr_arg_t *)arg;
    sim_yield(self, sim_timeout(a->env, a->delay));
    sim_event_t *req = sim_priority_resource_request(a->r, a->prio);
    sim_yield(self, req);
    a->order[(*a->idx)++] = a->id;
    sim_yield(self, sim_timeout(a->env, 5.0));
    sim_priority_resource_release(a->r, req);
}

static void test_priority_resource(void) {
    sim_env_t *env = sim_env_create();
    sim_priority_resource_t *r = sim_priority_resource_create(env, 1);
    int order[4]; int idx = 0;
    /* All four arrive in [0, 1), but priorities differ. The first to
     * arrive (id=0) gets in immediately. The other three queue and
     * are served in priority order: id=3 (prio 0), then id=1 (prio 1),
     * then id=2 (prio 2). */
    pr_arg_t a0 = { env, r, order, &idx, 0, 5, 0.0 };
    pr_arg_t a1 = { env, r, order, &idx, 1, 1, 0.1 };
    pr_arg_t a2 = { env, r, order, &idx, 2, 2, 0.2 };
    pr_arg_t a3 = { env, r, order, &idx, 3, 0, 0.3 };
    sim_process(env, pr_worker, &a0);
    sim_process(env, pr_worker, &a1);
    sim_process(env, pr_worker, &a2);
    sim_process(env, pr_worker, &a3);
    sim_run(env);
    CHECK(idx == 4);
    CHECK(order[0] == 0);
    CHECK(order[1] == 3);
    CHECK(order[2] == 1);
    CHECK(order[3] == 2);
    sim_priority_resource_destroy(r);
    sim_env_destroy(env);
}

/* ---- 3. PreemptiveResource: a higher-priority request evicts a holder ---- */
typedef struct {
    sim_env_t                *env;
    sim_preemptive_resource_t *r;
    int                       prio;
    double                    delay;
    double                    work;
    int                      *finished;
    int                       id;
    int                      *evicted_id;
} pre_arg_t;

static void pre_worker(sim_process_t *self, void *arg) {
    pre_arg_t *a = (pre_arg_t *)arg;
    sim_yield(self, sim_timeout(a->env, a->delay));
    sim_event_t *req = sim_preemptive_resource_request(
        a->r, self, a->prio, 1 /* preempt */);
    void *r = sim_yield(self, req);
    if (sim_process_was_interrupted(self)) {
        /* Waiting in queue; got interrupted ourselves (shouldn't here). */
        *a->evicted_id = a->id;
        sim_preemptive_resource_release(a->r, req);
        return;
    }
    (void)r;
    /* Try to work; might be preempted mid-work. */
    void *r2 = sim_yield(self, sim_timeout(a->env, a->work));
    if (sim_process_was_interrupted(self)) {
        *a->evicted_id = a->id;
    } else {
        a->finished[a->id] = 1;
    }
    sim_preemptive_resource_release(a->r, req);
    (void)r2;
}

static void test_preemptive_resource(void) {
    sim_env_t *env = sim_env_create();
    sim_preemptive_resource_t *r = sim_preemptive_resource_create(env, 1);
    int finished[2] = {0, 0};
    int evicted = -1;
    /* worker 0: low-priority (10), arrives at 0, work=20 */
    pre_arg_t a0 = { env, r, 10, 0.0, 20.0, finished, 0, &evicted };
    /* worker 1: high-priority (1), arrives at 5, work=5 */
    pre_arg_t a1 = { env, r, 1,  5.0,  5.0, finished, 1, &evicted };
    sim_process(env, pre_worker, &a0);
    sim_process(env, pre_worker, &a1);
    sim_run(env);
    /* worker 0 should have been evicted; worker 1 finishes. */
    CHECK(evicted == 0);
    CHECK(finished[1] == 1);
    sim_preemptive_resource_destroy(r);
    sim_env_destroy(env);
}

/* ---- 4. FilterStore: get with predicate ---- */
typedef struct { int kind; int payload; } widget_t;

static int is_kind(void *item, void *user) {
    widget_t *w = (widget_t *)item;
    int wanted = (int)(intptr_t)user;
    return w->kind == wanted;
}

static void test_filter_store(void) {
    sim_env_t *env = sim_env_create();
    sim_filter_store_t *s = sim_filter_store_create(env, 0);

    widget_t a = {1, 100};
    widget_t b = {2, 200};
    widget_t c = {1, 101};

    /* Buffer items first; later get specific kinds. */
    sim_filter_store_put(s, &a);
    sim_filter_store_put(s, &b);
    sim_filter_store_put(s, &c);
    CHECK(sim_filter_store_count(s) == 3);

    /* Want kind=2 first. Buffer contains [a, b, c]; b matches.
     * The get is fulfilled synchronously so the value is available
     * before sim_run, but the event itself is only "scheduled" (not
     * yet processed) — read the value via the event pointer now. */
    sim_event_t *g1 = sim_filter_store_get(s, is_kind, (void *)(intptr_t)2);
    CHECK(((widget_t *)sim_event_value(g1))->payload == 200);
    CHECK(sim_filter_store_count(s) == 2);

    sim_event_t *g2 = sim_filter_store_get(s, is_kind, (void *)(intptr_t)1);
    CHECK(((widget_t *)sim_event_value(g2))->payload == 100);

    sim_event_t *g3 = sim_filter_store_get(s, is_kind, (void *)(intptr_t)1);
    CHECK(((widget_t *)sim_event_value(g3))->payload == 101);

    /* Drain pending events. After this, get-event pointers are
     * recycled and stale; don't touch them. */
    sim_run(env);

    sim_filter_store_destroy(s);
    sim_env_destroy(env);
}

/* FilterStore: waiter pattern (no item matches initially) */
typedef struct {
    sim_env_t          *env;
    sim_filter_store_t *s;
    int                 wanted_kind;
    int                *got_payload;
} fs_proc_arg_t;

static void fs_consumer(sim_process_t *self, void *arg) {
    fs_proc_arg_t *a = (fs_proc_arg_t *)arg;
    void *item = sim_yield(self,
        sim_filter_store_get(a->s, is_kind, (void *)(intptr_t)a->wanted_kind));
    *a->got_payload = ((widget_t *)item)->payload;
}

static void fs_producer(sim_process_t *self, void *arg) {
    fs_proc_arg_t *a = (fs_proc_arg_t *)arg;
    sim_yield(self, sim_timeout(a->env, 1.0));
    static widget_t junk = {9, 999};
    static widget_t hit  = {7, 777};
    sim_filter_store_put(a->s, &junk);   /* shouldn't match */
    sim_yield(self, sim_timeout(a->env, 1.0));
    sim_filter_store_put(a->s, &hit);    /* matches */
}

static void test_filter_store_waiter(void) {
    sim_env_t *env = sim_env_create();
    sim_filter_store_t *s = sim_filter_store_create(env, 0);
    int got = 0;
    fs_proc_arg_t a = { env, s, 7, &got };
    sim_process(env, fs_consumer, &a);
    sim_process(env, fs_producer, &a);
    sim_run(env);
    CHECK(got == 777);
    CHECK(sim_filter_store_count(s) == 1);  /* junk is still buffered */
    sim_filter_store_destroy(s);
    sim_env_destroy(env);
}

/* ---- 5. PriorityStore: get returns lowest-priority value ---- */
static void test_priority_store(void) {
    sim_env_t *env = sim_env_create();
    sim_priority_store_t *s = sim_priority_store_create(env, 0);
    sim_priority_store_put(s, 5, (void *)(intptr_t)5);
    sim_priority_store_put(s, 1, (void *)(intptr_t)1);
    sim_priority_store_put(s, 3, (void *)(intptr_t)3);
    sim_priority_store_put(s, 1, (void *)(intptr_t)11);  /* tied with 1 */

    sim_event_t *g1 = sim_priority_store_get(s);
    CHECK((intptr_t)sim_event_value(g1) == 1);   /* first prio-1 */
    sim_event_t *g2 = sim_priority_store_get(s);
    CHECK((intptr_t)sim_event_value(g2) == 11);  /* tied */
    sim_event_t *g3 = sim_priority_store_get(s);
    CHECK((intptr_t)sim_event_value(g3) == 3);
    sim_event_t *g4 = sim_priority_store_get(s);
    CHECK((intptr_t)sim_event_value(g4) == 5);
    CHECK(sim_priority_store_count(s) == 0);

    sim_run(env);
    sim_priority_store_destroy(s);
    sim_env_destroy(env);
}

/* ---- 6. RealtimeEnvironment: 10 sim units in factor=0.01 wall sec ---- */
static void rt_proc(sim_process_t *self, void *arg) {
    sim_env_t *env = (sim_env_t *)arg;
    for (int i = 0; i < 10; i++) {
        sim_yield(self, sim_timeout(env, 1.0));
    }
}

static void test_realtime(void) {
    sim_env_t *env = sim_env_create();
    sim_env_set_realtime(env, 0.01, 0);  /* 10ms per sim unit */
    sim_process(env, rt_proc, env);
    double t0 = wall_now();
    sim_run(env);
    double dt = wall_now() - t0;
    /* Expect ~100ms wall; allow generous slack for CI. */
    CHECK(dt >= 0.080);
    CHECK(dt <= 0.500);
    sim_env_destroy(env);
}

/* ---- 7. Lazy done event ---- */
static void quick_proc(sim_process_t *self, void *arg) {
    sim_env_t *env = (sim_env_t *)arg;
    sim_yield(self, sim_timeout(env, 1.0));
}

static int waiter_woke;
static void waiter_proc(sim_process_t *self, void *arg) {
    sim_process_t *child = (sim_process_t *)arg;
    sim_yield(self, sim_process_event(child));  /* lazy alloc here */
    waiter_woke = 1;
}

static void test_lazy_done_waited(void) {
    /* Spawn child, ask for its event BEFORE it exits, then yield. */
    sim_env_t *env = sim_env_create();
    sim_process_t *child = sim_process(env, quick_proc, env);
    waiter_woke = 0;
    sim_process(env, waiter_proc, child);
    sim_run(env);
    CHECK(waiter_woke == 1);
    sim_env_destroy(env);
}

static void test_lazy_done_after_exit(void) {
    /* Spawn child, run sim until child exits, THEN ask for its event.
     * Should return a pre-succeeded event. */
    sim_env_t *env = sim_env_create();
    sim_process_t *child = sim_process(env, quick_proc, env);
    sim_run(env);     /* child runs to completion (lazy: never asked) */
    sim_event_t *done = sim_process_event(child);
    CHECK(done != NULL);
    CHECK(sim_event_triggered(done));    /* succeeded immediately */
    sim_env_destroy(env);
}

static void test_lazy_done_never_asked(void) {
    /* Fire-and-forget: spawn many, never call sim_process_event. The
     * test just has to complete without crashing. */
    sim_env_t *env = sim_env_create();
    for (int i = 0; i < 100; i++) sim_process(env, quick_proc, env);
    sim_run(env);
    sim_env_destroy(env);
}

int main(void) {
    test_interrupt();
    test_priority_resource();
    test_preemptive_resource();
    test_filter_store();
    test_filter_store_waiter();
    test_priority_store();
    test_realtime();
    test_lazy_done_waited();
    test_lazy_done_after_exit();
    test_lazy_done_never_asked();
    printf("OK\n");
    return 0;
}
