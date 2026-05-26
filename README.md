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

| Concept             | SimPy                       | simpyc                            |
|---------------------|-----------------------------|-----------------------------------|
| Env                 | `Environment`               | `sim_env_create / sim_run[_until]`|
| Time                | `env.now`                   | `sim_now`                         |
| Event               | `env.event()`               | `sim_event`                       |
| Timeout             | `env.timeout(t)`            | `sim_timeout`                     |
| Trigger             | `e.succeed()`               | `sim_event_succeed / _fail`       |
| Process             | `env.process(g)`            | `sim_process` + `sim_yield`       |
| Interrupt           | `process.interrupt(cause)`  | `sim_process_interrupt`           |
| AllOf               | `e1 & e2`                   | `sim_all_of`                      |
| AnyOf               | `e1 \| e2`                  | `sim_any_of`                      |
| Resource            | `Resource(N)`               | `sim_resource_create`             |
| PriorityResource    | `PriorityResource(N)`       | `sim_priority_resource_create`    |
| PreemptiveResource  | `PreemptiveResource(N)`     | `sim_preemptive_resource_create`  |
| Container           | `Container(c)`              | `sim_container_create`            |
| Store               | `Store(c)`                  | `sim_store_create`                |
| FilterStore         | `FilterStore(c)` + filter   | `sim_filter_store_create`         |
| PriorityStore       | `PriorityStore(c)`          | `sim_priority_store_create`       |
| RealtimeEnvironment | `RealtimeEnvironment(...)`  | `sim_env_set_realtime`            |

See [include/simpyc.h](include/simpyc.h) for the full public API and
[examples/](examples) for runnable ports of SimPy's tutorial programs.

## Layout

```
include/simpyc.h             # public API (opaque types)
src/coro.h
src/coro_arm64.S             # ARM64 (AAPCS) context switch
src/coro_amd64.S             # x86_64 (SysV)  context switch
src/coro.c                   # arch-independent init
src/heap.c                   # min-heap event queue
src/event.c                  # events, callbacks, recycling
src/env.c                    # scheduler loop + realtime pacing
src/process.c                # processes + interrupt
src/cond.c                   # AllOf / AnyOf
src/resource.c               # capacity-N resource (FIFO)
src/priority_resource.c      # priority-ordered queue
src/preemptive_resource.c    # priority + preemption
src/container.c              # continuous container
src/store.c                  # discrete store
src/filter_store.c           # predicate-based get
src/priority_store.c         # priority-ordered items
```

## Caveats

- **Stale events still pop from the heap.** If a process is interrupted
  while waiting on a timeout, the timeout still fires later (callbacks
  short-circuit, but `sim_now` advances past it). Matches SimPy's
  behavior; the user-visible action is correct, only the final
  `sim_now` is inflated. Add an explicit cancel mechanism if needed.
- **Event handles must not be touched after processing.** Recyclable
  events (timeouts, container/store ops) get pooled in
  `_sim_event_run_callbacks`; after yield, read the value via the
  `sim_yield` return value, not via `sim_event_value` on the now-stale
  pointer. Resource `req` handles are explicitly recycled by
  `*_release`, so don't access them after release either.
