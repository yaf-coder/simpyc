/* Port of SimPy's "carwash" tutorial.
 *
 * 2-bay carwash; cars arrive at random intervals and queue for a bay.
 * Each wash takes a fixed time. We log a small trace. */

#include "simpyc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define BAYS         2
#define WASH_TIME    5.0
#define ARRIVAL_MEAN 2.0
#define N_CARS       8

typedef struct { sim_env_t *env; sim_resource_t *bays; int id; } car_arg_t;
typedef struct { sim_env_t *env; sim_resource_t *bays; } gen_arg_t;

static double expovariate(double mean) {
    /* Inverse-CDF sample of an exponential. rand() is fine for a demo. */
    double u = (rand() + 1.0) / ((double)RAND_MAX + 2.0);
    return -mean * log(u);
}

static void car(sim_process_t *self, void *arg) {
    car_arg_t *a = (car_arg_t *)arg;
    printf("car %d arrives at %.2f\n", a->id, sim_now(a->env));
    sim_event_t *req = sim_resource_request(a->bays);
    sim_yield(self, req);
    printf("car %d starts wash at %.2f\n", a->id, sim_now(a->env));
    sim_yield(self, sim_timeout(a->env, WASH_TIME));
    sim_resource_release(a->bays, req);
    printf("car %d leaves at  %.2f\n", a->id, sim_now(a->env));
    free(a);
}

static void generator(sim_process_t *self, void *arg) {
    gen_arg_t *g = (gen_arg_t *)arg;
    for (int i = 0; i < N_CARS; i++) {
        sim_yield(self, sim_timeout(g->env, expovariate(ARRIVAL_MEAN)));
        car_arg_t *ca = (car_arg_t *)malloc(sizeof(*ca));
        ca->env = g->env; ca->bays = g->bays; ca->id = i;
        sim_process(g->env, car, ca);
    }
}

int main(void) {
    srand(42);
    sim_env_t *env = sim_env_create();
    sim_resource_t *bays = sim_resource_create(env, BAYS);
    gen_arg_t g = { env, bays };
    sim_process(env, generator, &g);
    sim_run(env);
    printf("done at %.2f\n", sim_now(env));
    sim_resource_destroy(bays);
    sim_env_destroy(env);
    return 0;
}
