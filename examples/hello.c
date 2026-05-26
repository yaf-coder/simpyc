/* Port of SimPy's first tutorial example.
 *
 * Python:
 *   def clock(env, name, tick):
 *       while True:
 *           print(name, env.now)
 *           yield env.timeout(tick)
 *   env = simpy.Environment()
 *   env.process(clock(env, 'fast', 0.5))
 *   env.process(clock(env, 'slow', 1.0))
 *   env.run(until=2)
 */

#include "simpyc.h"

#include <stdio.h>

typedef struct { sim_env_t *env; const char *name; double tick; } clock_arg_t;

static void clock_proc(sim_process_t *self, void *arg) {
    clock_arg_t *a = (clock_arg_t *)arg;
    for (;;) {
        printf("%s at %.1f\n", a->name, sim_now(a->env));
        sim_yield(self, sim_timeout(a->env, a->tick));
    }
}

int main(void) {
    sim_env_t *env = sim_env_create();
    clock_arg_t fast = { env, "fast", 0.5 };
    clock_arg_t slow = { env, "slow", 1.0 };
    sim_process(env, clock_proc, &fast);
    sim_process(env, clock_proc, &slow);
    sim_run_until(env, 2.0);
    sim_env_destroy(env);
    return 0;
}
