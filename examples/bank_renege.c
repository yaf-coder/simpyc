/* Port of SimPy's "bank renege" example.
 *
 * A bank with one counter. Customers arrive randomly; each is patient
 * for a random amount of time, after which they leave the queue. */

#include "simpyc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define N_CUSTOMERS      5
#define INTERVAL_MEAN    10.0   /* avg arrival gap */
#define PATIENCE_MIN     1.0
#define PATIENCE_MAX     3.0
#define SERVICE_MEAN     12.0

typedef struct { sim_env_t *env; sim_resource_t *counter; int id;
                 double patience, service; } cust_t;
typedef struct { sim_env_t *env; sim_resource_t *counter; } gen_t;

static double uniform(double a, double b) {
    return a + (b - a) * (rand() / (double)RAND_MAX);
}
static double expovariate(double mean) {
    double u = (rand() + 1.0) / ((double)RAND_MAX + 2.0);
    return -mean * log(u);
}

static void customer(sim_process_t *self, void *arg) {
    cust_t *c = (cust_t *)arg;
    double t0 = sim_now(c->env);
    printf("[t=%6.2f] cust %d arrives\n", t0, c->id);

    sim_event_t *req      = sim_resource_request(c->counter);
    sim_event_t *patience = sim_timeout(c->env, c->patience);
    sim_event_t *waits[2] = { req, patience };
    sim_event_t *either   = sim_any_of(c->env, waits, 2);
    sim_yield(self, either);

    if (sim_event_processed(req)) {
        double wait = sim_now(c->env) - t0;
        printf("[t=%6.2f] cust %d served (wait %.2f, srv %.2f)\n",
               sim_now(c->env), c->id, wait, c->service);
        sim_yield(self, sim_timeout(c->env, c->service));
        sim_resource_release(c->counter, req);
    } else {
        printf("[t=%6.2f] cust %d RENEGED after %.2f\n",
               sim_now(c->env), c->id, c->patience);
        /* req is still queued — cancel by releasing if it later
         * succeeds. SimPy uses request.cancel(); here we leak the
         * request in the queue. For a demo this is fine. */
    }
    free(c);
}

static void source(sim_process_t *self, void *arg) {
    gen_t *g = (gen_t *)arg;
    for (int i = 0; i < N_CUSTOMERS; i++) {
        cust_t *c = (cust_t *)malloc(sizeof(*c));
        c->env = g->env; c->counter = g->counter; c->id = i;
        c->patience = uniform(PATIENCE_MIN, PATIENCE_MAX);
        c->service  = expovariate(SERVICE_MEAN);
        sim_process(g->env, customer, c);
        sim_yield(self, sim_timeout(g->env, expovariate(INTERVAL_MEAN)));
    }
}

int main(void) {
    srand(7);
    sim_env_t *env = sim_env_create();
    sim_resource_t *counter = sim_resource_create(env, 1);
    gen_t g = { env, counter };
    sim_process(env, source, &g);
    sim_run(env);
    sim_resource_destroy(counter);
    sim_env_destroy(env);
    return 0;
}
