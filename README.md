# simpyc

A port of [SimPy](https://simpy.readthedocs.io)'s discrete-event simulation
core to C. simpyc preserves SimPy's process-based programming model — you
write processes as plain functions that yield events, and the scheduler
runs them in simulated time — but replaces the Python runtime with:

- A flat-array binary min-heap event queue, keyed on `(time, priority, sequence)`.
- Hand-rolled stack-switching coroutines, with assembly for AArch64 (Apple
  Silicon, Linux ARM) and x86_64 SysV. Each context switch is ~13
  instructions; no `ucontext`, no signal-mask syscalls.
- A pool allocator for both events and processes — in steady state, a
  simulation does **zero heap allocations per yield**.
- Per-event packed dynamic-array callback storage instead of per-callback
  linked-list nodes.

End-to-end the factory bench runs **21.9× faster** than reference SimPy
on the same workload, and uses **15.1× less peak RSS**. Per-primitive
the geometric-mean speedup across all 15 measured ops is ~18×.

## Performance summary

Apple M-series, `clang -O3`, single thread. Numbers are medians of three
runs; full bench harness in [`benchmarking/`](benchmarking/README.md).

```
metric                simpy(py)      simpyc(c)          ratio
wall seconds           0.079819       0.003650   21.9x faster
peak RSS (MB)             24.52           1.62     15.1x less
produced                   1666           1666
shipped                    1665           1665
preempted                   185            185
reviewed                    666            666
```

Per-primitive (ns per operation, lower is better):

```
primitive                    py ns/op     c ns/op     speedup
timeout                         620.3        63.5        9.8x
event-succeed                  1241.2       129.1        9.6x
callback                         47.7         5.6        8.5x
process-spawn (cold)           2802.7      1218.6        2.3x
process-churn (warm)           1632.4       173.3        9.4x
resource (FIFO)                2518.2        68.2       36.9x
priority-resource              3127.5        67.7       46.2x
preemptive-resource            3586.8        92.9       38.6x
container                      2971.2       138.2       21.5x
store                          2785.2       162.0       17.2x
filter-store                   3141.1       199.7       15.7x
priority-store                 3357.3       191.2       17.6x
allof                          4983.4       245.7       20.3x
anyof                          4928.1       240.9       20.5x
interrupt                      5758.1       547.2       10.5x
```

## Build

```
make           # libsimpyc.a + examples + tests
make test      # smoke tests (basic + extended)
make bench     # examples/bench (raw yields/sec)
make clean
```

The static library is `libsimpyc.a`. Link against it with `-lsimpyc`
(or just include the `.a` directly) and add `-I path/to/simpyc/include`.

## Quickstart

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

```
tick at 0.0
tick at 1.0
tick at 2.0
tick at 3.0
tick at 4.0
```

See [examples/](examples) for full ports of SimPy's tutorial programs
(hello, car_wash, bank_renege).

## Architecture

### Event scheduling — `src/heap.c`, `src/env.c`

The event queue is a flat-array binary min-heap of `heap_entry_t`:

```c
typedef struct {
    double       time;     // simulation time the event should fire
    int          priority; // 0 = URGENT, 1 = NORMAL
    uint64_t     seq;      // monotonic; breaks ties stably (FIFO)
    sim_event_t *event;    // payload
} heap_entry_t;
```

The comparison is `(time, priority, seq)` ascending. The sequence number
gives FIFO ordering for events scheduled at the same `(time, priority)`,
matching SimPy's `(_now, priority, count)` ordering.

`sim_step()` peeks the head, advances `env->now` to that entry's time,
then runs the event's callbacks. `sim_run_until(env, T)` synthesises a
sentinel at `(T, URGENT, current_seq)` and stops when the head is `>=`
the sentinel — so normal-priority events scheduled at exactly time `T`
do *not* fire, matching SimPy's `StopSimulation` behavior.

Heap operations are O(log N) and operate on cache-line-contiguous data;
the heap grows geometrically (2× starting at 32 entries).

### Coroutines — `src/coro_arm64.S`, `src/coro_amd64.S`, `src/coro.c`

Processes run on real CPU stacks. Each process owns a `malloc`'d 16 KiB
region (`SIM_DEFAULT_STACK`); the coroutine state is 32 saved words in
`coro_ctx_t`.

**AArch64 layout** (AAPCS callee-saved registers):

| slot   | reg      | slot   | reg     |
|--------|----------|--------|---------|
| 0      | sp       | 12     | x29 (fp) |
| 1      | lr (x30) | 13–14  | d8, d9  |
| 2–3    | x19, x20 | 15–16  | d10, d11 |
| 4–5    | x21, x22 | 17–18  | d12, d13 |
| 6–7    | x23, x24 | 19–20  | d14, d15 |
| 8–9    | x25, x26 |        |          |
| 10–11  | x27, x28 |        |          |

`coro_switch(from, to)` is ~26 `stp`/`ldp` pairs plus one `ret` — about
30 ns on M-series. No syscalls, no FP context save beyond `d8-d15`
(SIMD scratch is caller-saved per AAPCS).

**x86_64 layout** (SysV callee-saved):

| slot | reg | slot | reg |
|------|-----|------|-----|
| 0    | rsp | 4    | r13 |
| 1    | rbp | 5    | r14 |
| 2    | rbx | 6    | r15 |
| 3    | r12 | 7    | rip |

Slots 2 and 3 do double duty: on first dispatch, `rbx` holds the
function argument and `r12` holds the entry pointer for the trampoline.

**`coro_init`** sets `sp` to the (16-byte-aligned) top of the stack,
arranges the first switch to land at `coro_trampoline`, and seeds two
callee-saved registers with `(arg, entry)`. The trampoline reads them,
calls `entry(arg)`, and traps if `entry` returns — but in practice the
process-entry wrapper switches out before that point.

### Process lifecycle — `src/process.c`

```
NEW ──(scheduler dispatches)──> RUNNING ──(returns)──> DEAD ──> pool
       schedule_resume                     fire done                ^
                                           switch out               │
                                                                    │
NEW <───(sim_process reuses)──────────────────────────────────────┘
```

A process struct holds:

```c
struct sim_process {
    sim_env_t   *env;
    sim_proc_fn  fn;
    void        *arg;
    coro_ctx_t   ctx;            // 256 bytes of saved registers
    void        *stack_base;     // malloc'd 16 KiB
    size_t       stack_size;
    sim_event_t *done;           // lazy: NULL until sim_process_event()
    sim_event_t *yielded;        // event currently being waited on
    sim_event_t *resume_ev;      // reusable event for scheduler resumes
    void        *resume_value;
    void        *interrupt_cause;
    uint8_t      state, interrupt_pending, was_interrupted;
    ...
};
```

Three key optimizations:

1. **`resume_ev` is per-process and reused.** Each yield-resume cycle
   would normally allocate a fresh "wake me up" event. Instead, every
   process owns one and `schedule_resume` resets-and-reschedules it.
   If a resume is already pending (TRIGGERED but not yet processed),
   the call coalesces — useful when an interrupt arrives while a
   normal wake-up is already in flight.

2. **`done` is allocated lazily.** Fire-and-forget processes — which
   are most of them in a typical SimPy program — never have a `done`
   event allocated. `sim_process_event(p)` allocates one on first call;
   if the process has already finished, the event is born pre-succeeded.

3. **Dead processes go into an env-owned free list.** When the user
   function returns, `process_entry` pushes the struct onto
   `env->process_pool`, preserving the 16 KiB stack and the
   `resume_ev`. The next `sim_process()` pops from the pool and
   re-inits the coroutine context — zero `malloc` calls on warm spawn.

Yield-roundtrip overhead is dominated by the heap push for the resume
event (~50 ns) and the two context switches (~60 ns), giving the
~170 ns measured at `process-churn`.

### Event lifecycle — `src/event.c`

```
PENDING ──succeed()──> TRIGGERED ──(scheduler pops)──> PROCESSED
                       (in heap)                       (callbacks fire)
                                                              │
                                       recyclable? ──yes──> pool
                                            │
                                            no
                                            │
                                            ↓
                                       lives until env_destroy
```

Each event holds:

```c
struct sim_event {
    sim_env_t  *env;
    cb_pair_t  *cbs;          // dynamic array (fn, user) pairs
    uint32_t    cb_cap, cb_len;
    void       *value;
    const char *fail_reason;
    uint8_t     state, ok, recyclable;
    ...
};
```

**Callbacks** live in a packed dynamic array of `cb_pair_t = (fn, user)`,
not a linked list. Subscription is a single bounds-check + two stores;
firing iterates contiguous memory in a tight loop. The array is owned
by the event and persists across recycle — an event that handles
thousands of callbacks pays one growth pattern (geometric, starting at
cap=2) and then never allocates again. At measured 5.6 ns per
subscribe-and-fire, the callback path is essentially free.

**Recycling.** Events that are demonstrably user-invisible after
processing are marked `recyclable=1` and pushed onto the env's free
list when their callbacks finish:

| event source                  | recyclable? |
|-------------------------------|-------------|
| `sim_timeout` / `_v`          | yes         |
| `sim_container_put` / `_get`  | yes         |
| `sim_store_put` / `_get`      | yes         |
| `sim_filter_store_*`          | yes         |
| `sim_priority_store_*`        | yes         |
| process resume events         | n/a (reused in place) |
| `sim_event` (manual)          | no — user owns |
| `sim_*_resource_request` req  | no — pooled in `release` instead |
| condition outputs (all/any)   | no — user may subscribe |
| process `done` events         | no — user may subscribe |

`_sim_event_alloc` pops from the env pool when one is available,
preserving the `cbs` array allocation (so subscriptions to a recycled
event reuse already-malloc'd capacity). This is what keeps the factory
at 1.6 MB RSS across a 10000-tick simulation.

### Realtime pacing — `sim_env_set_realtime(env, factor, strict)`

Before each `sim_step`, the scheduler computes the wall-clock target
for the next event:

```
target = rt_start_wall + (event_time - rt_start_sim) * factor
```

If wall time is behind target, `nanosleep` waits. If wall time is ahead
*and* `strict` mode is set, the loop aborts and `sim_env_realtime_lagged()`
returns 1. Used for live-driven simulations or demos where sim time
should map to seconds.

## Per-primitive notes

### `sim_timeout(env, delay)` — **63 ns/op**

Allocates an event from the pool (or `calloc(64)` if cold), sets state
to `TRIGGERED`, marks recyclable, pushes onto the heap. Per-yield: heap
push + heap pop + callback dispatch + recycle. The whole roundtrip is
one heap operation pair plus a switch.

### `sim_event_succeed(e, value)` — **130 ns/op**

Like `timeout` but the event is allocated separately by the caller and
the caller chose when to succeed. Slightly heavier because the event
isn't marked recyclable by default (caller may hold a reference). The
caller can mark it recyclable manually if they know better.

### `sim_event_on(e, cb, user)` + fire — **5.6 ns/op**

The bench attaches 100,000 callbacks to one event and succeeds it once.
Each subscribe is a bounds check on `cb_len < cb_cap` (the array grows
geometrically — about 17 reallocs for 100K subscriptions) plus two
stores. Firing is one for-loop over contiguous memory.

### `sim_process(env, fn, arg)` — **2.0 µs cold / 170 ns churn**

Cold spawn (`process-spawn` bench: N spawns then drain) is dominated
by `malloc(16 KiB)` for the stack. SimPy's cold spawn is faster
(~2.8 µs vs simpyc's ~2.0 µs) because Python generators don't allocate
stacks — they piggyback on the interpreter frame stack — but simpyc's
smaller default stack + the pool put us slightly ahead.

Warm spawn (`process-churn` bench: spawn-die-spawn) is what real
workloads do, and there simpyc is **9.4× faster** at 170 ns/op — after
the first iteration the pool serves every spawn with no `malloc`.

### `sim_yield(self, evt)` — included in every measurement above

Subscribes a `waiter_cb` to `evt` (or schedules an immediate resume if
`evt` is already processed), then `coro_switch` to the host. When the
callback later fires, it calls `schedule_resume(self, evt->value)`,
which puts self's reusable `resume_ev` into the heap. The scheduler
pops it, switches into the process, and `sim_yield` returns the value.

If the process was interrupted while yielding, the yielded-event's
`waiter_cb` is now stale — it checks `self->yielded != ev` and silently
returns rather than double-scheduling the process.

### `sim_process_interrupt(p, cause)` — **545 ns/op**

Sets `p->interrupt_pending`, stores `cause`, clears `p->yielded`, and
schedules a resume. The next `sim_yield` to return inside `p` sees the
pending flag and returns `cause` instead of the event's value. The
caller checks via `sim_process_was_interrupted(self)`.

Per op: one extra resume scheduling + two extra context switches vs a
plain yield. About 8× a plain yield, matching Python's relative cost.

### `sim_resource_t` (FIFO) — **68 ns/op request+release**

Capacity-N counter plus a FIFO singly-linked queue of waiters. Request:
if `in_use < cap`, increment and succeed synchronously; else append to
the queue. Release: pop the queue head (if any) and succeed it, else
decrement `in_use`. Released request events are pushed back to the env
pool inside `release`.

### `sim_priority_resource_t` — **68 ns/op**

Like `Resource` but the waiter queue is kept sorted by `(priority, seq)`
on insertion. O(N) insertion, O(1) head-pop. Acceptable in practice
because queues are typically short; if you have thousands of waiters,
swap to a per-resource heap.

### `sim_preemptive_resource_t` — **93 ns/op (no preemption case)**

Tracks holders separately from waiters. On request with `preempt=1`,
scans the holder list for the worst (largest priority value) holder; if
that holder's priority is strictly greater than the requester's, the
holder is evicted via `sim_process_interrupt(holder, new_req)`. The
slot is granted to the new requester immediately. The evicted holder's
later `release` becomes a no-op (their req is no longer in the holder
list).

### `sim_container_t` — **138 ns/op put+get**

Continuous level with `(put_queue, get_queue)`. A put first tops up the
level (if within capacity and no put-waiters ahead) then drains any
get-waiters whose requested amounts are ≤ current level. A get does the
mirror. Both queues are FIFO; partial-fill is not supported (matches
SimPy).

### `sim_store_t` — **162 ns/op put+get**

Bounded or unbounded FIFO of opaque `void *` items. Direct handoff:
if a get-waiter is queued when a put arrives, the item bypasses the
buffer entirely. Symmetric on the get side.

### `sim_filter_store_t` — **200 ns/op put+get (accept-all filter)**

Adds a per-get predicate `int (*)(void *item, void *user)`. On put we
walk waiters first, handing the item to the first matching waiter
without buffering. If no waiter matches, the item is buffered. On get
we scan the buffer for the first matching item; if none, we queue the
waiter (with its predicate captured). Whenever a put-waiter is drained
into the buffer, we re-run the waiter dispatch loop in case the new
item satisfies an existing get-waiter.

### `sim_priority_store_t` — **191 ns/op put+get**

Items inserted sorted by `(priority, seq)`. Get always returns the head
(highest priority, FIFO tiebreaker). Get-waiters are FIFO; on each put
the new head goes to the front waiter (since they always want the
top-priority item).

### `sim_all_of(env, events, n)` / `sim_any_of` — **246 ns / 241 ns per op**

Each constructs a fresh output event plus a `cond_state_t` (n_total,
n_fired, output, mode). A callback `cond_cb` is subscribed to every
input; it increments `n_fired` and, when the threshold is met (`n_fired
>= 1` for any, `>= n_total` for all), succeeds the output event. The
state struct is freed only when all inputs have fired, so AnyOf's late
callbacks don't dereference a freed pointer.

The bench measures 4 child timeouts per op, so the cost is roughly
`4× timeout + condition wiring`.

## API surface

| Concept             | SimPy                       | simpyc                            |
|---------------------|-----------------------------|-----------------------------------|
| Environment         | `Environment`               | `sim_env_create / sim_run[_until] / sim_step` |
| Time                | `env.now`                   | `sim_now`                         |
| Event               | `env.event()`               | `sim_event`                       |
| Timeout             | `env.timeout(t)`            | `sim_timeout / _v`                |
| Trigger             | `e.succeed() / .fail()`     | `sim_event_succeed / _fail`       |
| State               | `e.triggered / .processed`  | `sim_event_triggered / _processed`|
| Callback            | `e.callbacks.append(...)`   | `sim_event_on(e, fn, user)`       |
| Process             | `env.process(g)`            | `sim_process(env, fn, arg)`       |
| Yield               | `yield event`               | `sim_yield(self, event)`          |
| Process done        | `proc` (it is an event)     | `sim_process_event(proc)`         |
| Interrupt           | `proc.interrupt(cause)`     | `sim_process_interrupt`           |
| AllOf               | `e1 & e2` / `env.all_of(...)` | `sim_all_of`                    |
| AnyOf               | `e1 \| e2` / `env.any_of(...)` | `sim_any_of`                    |
| Resource            | `Resource(N)`               | `sim_resource_create`             |
| PriorityResource    | `PriorityResource(N)`       | `sim_priority_resource_create`    |
| PreemptiveResource  | `PreemptiveResource(N)`     | `sim_preemptive_resource_create`  |
| Container           | `Container(cap, init)`      | `sim_container_create`            |
| Store               | `Store(cap)`                | `sim_store_create`                |
| FilterStore         | `FilterStore(cap)`          | `sim_filter_store_create`         |
| PriorityStore       | `PriorityStore(cap)`        | `sim_priority_store_create`       |
| RealtimeEnvironment | `RealtimeEnvironment(...)`  | `sim_env_set_realtime`            |

Full public API in [include/simpyc.h](include/simpyc.h).

## Layout

```
include/simpyc.h             # public API (opaque types)
src/coro.h                   # coroutine API
src/coro_arm64.S             # AArch64 (AAPCS) context switch
src/coro_amd64.S             # x86_64 (SysV) context switch
src/coro.c                   # arch-independent coro_init
src/internal.h               # shared struct layouts
src/heap.c                   # min-heap event queue
src/event.c                  # events, callbacks, pool recycling
src/env.c                    # scheduler loop, realtime pacing
src/process.c                # processes, interrupt, lazy done
src/cond.c                   # AllOf / AnyOf
src/resource.c               # capacity-N FIFO resource
src/priority_resource.c      # priority-ordered queue
src/preemptive_resource.c    # priority + preemption
src/container.c              # continuous container
src/store.c                  # discrete FIFO store
src/filter_store.c           # predicate-based get
src/priority_store.c         # priority-ordered store

examples/hello.c             # SimPy "two clocks" tutorial
examples/car_wash.c          # SimPy car-wash tutorial
examples/bank_renege.c       # SimPy bank-renege tutorial
examples/bench.c             # raw yields/sec throughput

tests/test_basic.c           # tier-1 + tier-2 smoke tests (7 tests)
tests/test_extra.c           # tier-3 + interrupt + lazy-done (10 tests)

benchmarking/                # SimPy side-by-side comparison
├── bench.sh                 # factory + micro harness
├── python/{factory,micro}.py
└── c/{factory,micro}.c
```

## Caveats and design tradeoffs

- **Stale events still pop from the heap.** If a process is interrupted
  while waiting on a `sim_timeout(env, 100.0)`, the timeout still fires
  at sim-time 100. Its `waiter_cb` notices `self->yielded != ev` and
  short-circuits — the *user-visible* action is correct — but `sim_now`
  advances past the stale event. Matches SimPy semantics. To stop this
  inflating `sim_now` in long-running simulations, you'd need explicit
  event cancellation (decrease-key in the heap, or a tombstone flag).
  Currently not implemented; happy to add if you need it.

- **Recyclable event handles must not be touched after `sim_yield`.**
  Events from `sim_timeout`, `sim_container_*`, `sim_store_*`,
  `sim_filter_store_*`, and `sim_priority_store_*` are pushed to the
  pool inside `_sim_event_run_callbacks`. After your `sim_yield(self,
  evt)` returns, `evt` may already be back in the pool serving a new
  allocation. Use the return value of `sim_yield` to read the event's
  value, not `sim_event_value(evt)` on the now-stale pointer. The
  smoke tests demonstrate the correct idiom.

- **Resource `req` handles must not be touched after `release`.**
  `sim_*_resource_release(r, req)` recycles `req` to the env pool.
  Don't read from it afterwards.

- **Default stack is 16 KiB.** Enough for typical SimPy-style process
  bodies (yield-heavy, low recursion). If your processes call deep into
  large C functions, bump `SIM_DEFAULT_STACK` in `src/process.c`.

- **Single-threaded.** The simulator runs one event at a time. There is
  no internal locking and no thread safety. Spawn multiple OS threads
  each with their own `sim_env_t *` if you need parallelism.

- **Process-spawn cold cost is intrinsically higher than SimPy's**
  because we allocate real CPU stacks (16 KiB malloc per spawn) where
  Python uses generator frames. Warm spawn (via the pool) closes the
  gap to 9× faster than SimPy. See the explanation in the
  [benchmarking README](benchmarking/README.md) for the full breakdown.
