/* Smoke tests: heap order, timeouts, callbacks, processes,
 * conditions, resource, container, store. */

#include "simpyc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do {                                   \
    if (!(cond)) {                                         \
        fprintf(stderr, "FAIL %s:%d: %s\n",                \
                __FILE__, __LINE__, #cond);                \
        exit(1);                                           \
    }                                                      \
} while (0)

/* ---- 1. Timeouts dispatched in time order ---- */
static int order_log[8];
static int order_idx;
static void log_cb(sim_event_t *e, void *user) {
    (void)e;
    order_log[order_idx++] = (int)(intptr_t)user;
}

static void test_timeouts(void) {
    sim_env_t *env = sim_env_create();
    sim_event_t *a = sim_timeout(env, 3.0);
    sim_event_t *b = sim_timeout(env, 1.0);
    sim_event_t *c = sim_timeout(env, 2.0);
    sim_event_t *d = sim_timeout(env, 1.0);     /* tie with b */
    sim_event_on(a, log_cb, (void *)(intptr_t)3);
    sim_event_on(b, log_cb, (void *)(intptr_t)1);
    sim_event_on(c, log_cb, (void *)(intptr_t)2);
    sim_event_on(d, log_cb, (void *)(intptr_t)4);
    order_idx = 0;
    sim_run(env);
    CHECK(order_idx == 4);
    CHECK(order_log[0] == 1);
    CHECK(order_log[1] == 4);   /* FIFO at same time */
    CHECK(order_log[2] == 2);
    CHECK(order_log[3] == 3);
    CHECK(sim_now(env) == 3.0);
    sim_env_destroy(env);
}

/* ---- 2. Process yields a timeout and resumes ---- */
static int counter;

static void inc_proc(sim_process_t *self, void *arg) {
    (void)arg;
    sim_env_t *env = (sim_env_t *)arg;
    for (int i = 0; i < 5; i++) {
        sim_yield(self, sim_timeout(env, 1.0));
        counter++;
    }
}

static void test_process_basic(void) {
    sim_env_t *env = sim_env_create();
    counter = 0;
    sim_process(env, inc_proc, env);
    sim_run(env);
    CHECK(counter == 5);
    CHECK(sim_now(env) == 5.0);
    sim_env_destroy(env);
}

/* ---- 3. Two processes alternating ---- */
static int trace[16];
static int trace_idx;

static void ping(sim_process_t *self, void *arg) {
    sim_env_t *env = (sim_env_t *)arg;
    for (int i = 0; i < 3; i++) {
        sim_yield(self, sim_timeout(env, 2.0));
        trace[trace_idx++] = 100 + i;
    }
}
static void pong(sim_process_t *self, void *arg) {
    sim_env_t *env = (sim_env_t *)arg;
    for (int i = 0; i < 3; i++) {
        sim_yield(self, sim_timeout(env, 3.0));
        trace[trace_idx++] = 200 + i;
    }
}

static void test_two_processes(void) {
    sim_env_t *env = sim_env_create();
    trace_idx = 0;
    sim_process(env, ping, env);
    sim_process(env, pong, env);
    sim_run(env);
    /* ping wakes at t=2,4,6 ; pong at t=3,6,9. At t=6 both fire, but
     * pong's timeout was scheduled first (sim time 3) so it dispatches
     * ahead of ping's (sim time 4). */
    CHECK(trace_idx == 6);
    CHECK(trace[0] == 100);
    CHECK(trace[1] == 200);
    CHECK(trace[2] == 101);
    CHECK(trace[3] == 201);
    CHECK(trace[4] == 102);
    CHECK(trace[5] == 202);
    CHECK(sim_now(env) == 9.0);
    sim_env_destroy(env);
}

/* ---- 4. AllOf / AnyOf ---- */
static void test_conditions(void) {
    sim_env_t *env = sim_env_create();
    sim_event_t *a = sim_timeout(env, 2.0);
    sim_event_t *b = sim_timeout(env, 5.0);
    sim_event_t *c = sim_timeout(env, 7.0);
    sim_event_t *evs[3] = {a, b, c};

    sim_event_t *any = sim_any_of(env, evs, 3);
    sim_event_t *all = sim_all_of(env, evs, 3);

    int fired_any = 0, fired_all = 0;
    sim_event_on(any, log_cb, (void *)(intptr_t)11);
    sim_event_on(all, log_cb, (void *)(intptr_t)22);
    order_idx = 0;
    sim_run(env);
    for (int i = 0; i < order_idx; i++) {
        if (order_log[i] == 11) fired_any = 1;
        if (order_log[i] == 22) fired_all = 1;
    }
    CHECK(fired_any);
    CHECK(fired_all);
    CHECK(sim_now(env) == 7.0);
    sim_env_destroy(env);
}

/* ---- 5. Resource: capacity-1, two requests serialize ---- */
static double finish_times[4];
static int    finish_idx;

typedef struct { sim_env_t *env; sim_resource_t *r; } work_arg_t;

static void worker(sim_process_t *self, void *arg) {
    work_arg_t *w = (work_arg_t *)arg;
    sim_event_t *req = sim_resource_request(w->r);
    sim_yield(self, req);
    sim_yield(self, sim_timeout(w->env, 5.0));   /* hold for 5 */
    sim_resource_release(w->r, req);
    finish_times[finish_idx++] = sim_now(w->env);
}

static void test_resource(void) {
    sim_env_t *env = sim_env_create();
    sim_resource_t *r = sim_resource_create(env, 1);
    work_arg_t w = { env, r };
    finish_idx = 0;
    sim_process(env, worker, &w);
    sim_process(env, worker, &w);
    sim_run(env);
    CHECK(finish_idx == 2);
    CHECK(finish_times[0] == 5.0);
    CHECK(finish_times[1] == 10.0);
    sim_resource_destroy(r);
    sim_env_destroy(env);
}

/* ---- 6. Container: producer/consumer ---- */
typedef struct { sim_env_t *env; sim_container_t *c; } ct_arg_t;

static int produced, consumed;

static void producer(sim_process_t *self, void *arg) {
    ct_arg_t *a = (ct_arg_t *)arg;
    for (int i = 0; i < 4; i++) {
        sim_yield(self, sim_timeout(a->env, 1.0));
        sim_yield(self, sim_container_put(a->c, 1.0));
        produced++;
    }
}

static void consumer(sim_process_t *self, void *arg) {
    ct_arg_t *a = (ct_arg_t *)arg;
    for (int i = 0; i < 4; i++) {
        sim_yield(self, sim_container_get(a->c, 1.0));
        consumed++;
    }
}

static void test_container(void) {
    sim_env_t *env = sim_env_create();
    sim_container_t *c = sim_container_create(env, 10.0, 0.0);
    ct_arg_t a = { env, c };
    produced = consumed = 0;
    sim_process(env, producer, &a);
    sim_process(env, consumer, &a);
    sim_run(env);
    CHECK(produced == 4);
    CHECK(consumed == 4);
    CHECK(sim_container_level(c) == 0.0);
    sim_container_destroy(c);
    sim_env_destroy(env);
}

/* ---- 7. Store: items pass through ---- */
typedef struct { sim_env_t *env; sim_store_t *s; } st_arg_t;
static int item_sum;

static void store_producer(sim_process_t *self, void *arg) {
    st_arg_t *a = (st_arg_t *)arg;
    for (intptr_t i = 1; i <= 5; i++) {
        sim_yield(self, sim_timeout(a->env, 1.0));
        sim_yield(self, sim_store_put(a->s, (void *)i));
    }
}

static void store_consumer(sim_process_t *self, void *arg) {
    st_arg_t *a = (st_arg_t *)arg;
    for (int i = 0; i < 5; i++) {
        void *item = sim_yield(self, sim_store_get(a->s));
        item_sum += (int)(intptr_t)item;
    }
}

static void test_store(void) {
    sim_env_t *env = sim_env_create();
    sim_store_t *s = sim_store_create(env, 0);
    st_arg_t a = { env, s };
    item_sum = 0;
    sim_process(env, store_producer, &a);
    sim_process(env, store_consumer, &a);
    sim_run(env);
    CHECK(item_sum == 1 + 2 + 3 + 4 + 5);
    sim_store_destroy(s);
    sim_env_destroy(env);
}

int main(void) {
    test_timeouts();
    test_process_basic();
    test_two_processes();
    test_conditions();
    test_resource();
    test_container();
    test_store();
    printf("OK\n");
    return 0;
}
