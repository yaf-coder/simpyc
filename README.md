# simpyc

A port of [SimPy](https://simpy.readthedocs.io)'s discrete-event simulation
core to C. Processes run on real stacks via a hand-rolled context switch
(ARM64 + x86_64 assembly), and events flow through a binary min-heap keyed
on `(time, priority, sequence)`.

On an M-series Mac, throughput is ~5M events/sec — roughly 15–30× faster
than the reference Python implementation for equivalent workloads.

## Build

```
make           # libsimpyc.a + examples + tests
make test      # smoke tests
make bench     # throughput benchmark
```

## Example

```c
#include "simpyc.h"
#include <stdio.h>

static void clock_proc(sim_process_t *self, void *arg) {
    sim_env_t *env = (sim_env_t *)arg;
    for (;;) {
        printf("tick at %.1f\n", sim_now(env));
        sim_yield(self, sim_timeout(env, 1.0));
    }
}

int main(void) {
    sim_env_t *env = sim_env_create();
    sim_process(env, clock_proc, env);
    sim_run_until(env, 5.0);
    sim_env_destroy(env);
}
```

## API surface

| Concept | SimPy            | simpyc                            |
|---------|------------------|-----------------------------------|
| Env     | `Environment`    | `sim_env_create / sim_run[_until]`|
| Time    | `env.now`        | `sim_now`                         |
| Event   | `env.event()`    | `sim_event`                       |
| Timeout | `env.timeout(t)` | `sim_timeout`                     |
| Trigger | `e.succeed()`    | `sim_event_succeed / _fail`       |
| Process | `env.process(g)` | `sim_process` + `sim_yield`       |
| AllOf   | `e1 & e2`        | `sim_all_of`                      |
| AnyOf   | `e1 \| e2`       | `sim_any_of`                      |
| Resource| `Resource(N)`    | `sim_resource_create`             |
| Container| `Container(c)`  | `sim_container_create`            |
| Store   | `Store(c)`       | `sim_store_create`                |

See [include/simpyc.h](include/simpyc.h) for the full public API and
[examples/](examples) for runnable ports of SimPy's tutorial programs.

## Layout

```
include/simpyc.h        # public API (opaque types)
src/coro.h
src/coro_arm64.S        # ARM64 (AAPCS) context switch
src/coro_amd64.S        # x86_64 (SysV)  context switch
src/coro.c              # arch-independent init
src/heap.c              # min-heap event queue
src/event.c             # events, callbacks, lifecycle
src/env.c               # scheduler loop
src/process.c           # processes (on coroutines)
src/cond.c              # AllOf / AnyOf
src/resource.c          # capacity-N resource
src/container.c         # continuous container
src/store.c             # discrete store
```

## What's not (yet) ported

- `PriorityResource` / `PreemptiveResource` (Resource is FIFO)
- `FilterStore`, `PriorityStore`
- `env.process(...).interrupt(...)` (cancellation of running processes)
- Real-time environment (`RealtimeEnvironment`)
- Event recycling (events live until `sim_env_destroy`; long simulations
  will accumulate memory)
