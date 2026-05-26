# benchmarking

Two benchmarks against upstream [SimPy](https://simpy.readthedocs.io):

1. **factory** — a full simulator that uses *every* primitive simpyc ports.
   Compares end-to-end wall time + peak RSS, and cross-checks that both
   implementations produce identical counts.
2. **micro** — tight-loop per-primitive benchmarks. Prints ns/op for
   each primitive in both implementations, and a comparison table.

## What's exercised

### factory ([factory.py](python/factory.py) / [factory.c](c/factory.c))

| primitive             | role in the factory                                        |
|-----------------------|------------------------------------------------------------|
| Environment / run     | the simulator itself; `run(until=T)` cuts the world off    |
| Timeout               | every delay (assembly, ship, refill, parts, maintenance)   |
| Event + succeed + on  | `target` event; succeeded when production hits the target; `on_target` callback records the time |
| Process               | refiller, parts_supplier, maintenance, workers, aggregator, dispatcher, reviewers, supervisor, watchdog |
| AnyOf                 | watchdog waits for `target | half-time-deadline`           |
| AllOf                 | watchdog then waits for a small `all_of([t1, t2])` bundle  |
| Resource              | **stations** — workers acquire a station (FIFO)            |
| PriorityResource      | **inspectors** — rush items get priority 0, regular 5      |
| PreemptiveResource    | **machine** — maintenance arrives priority 0+preempt and evicts the worker holding it (worker counts it as `preempted`) |
| Container             | **materials** — refiller puts, workers get                 |
| Store                 | **finished** — buffer between assembly and shipping        |
| FilterStore           | **parts** — workers pull parts of their own `kind` only    |
| PriorityStore         | **ship_q** — aggregator puts with `priority`; dispatcher pulls highest-priority first |
| Process.interrupt     | **supervisor** every 30 sim units interrupts both reviewers; reviewers count |

Defaults produce 1666 products with 185 preemptions and 666 supervisor-induced reviews.
Both implementations agree on every count.

### micro ([micro.py](python/micro.py) / [micro.c](c/micro.c))

One bench per primitive, run in isolation with `N=100000` (with `process-spawn` scaled down to `N/10` because each spawn allocates a real stack).

## Run

```
./bench.sh             # factory + micro
./bench.sh factory     # only factory; forwards args (e.g. --sim-time=200000)
./bench.sh micro -n 200000
```

First invocation creates `.venv` and `pip install simpy`. Subsequent runs reuse it.

## Results — factory (Apple M-series, `-O3`)

```
metric                simpy(py)      simpyc(c)          ratio
------                ---------      ---------          -----
wall seconds           0.079135       0.005794   13.7x faster
peak RSS (MB)             24.50           1.69     14.5x less
produced                   1666           1666
shipped                    1665           1665
preempted                   185            185
reviewed                    666            666
```

## Results — per-primitive (ns per op, lower is better)

```
primitive                    ops    py ns/op     c ns/op     speedup
---------------------- --------- ----------- ----------- -----------
timeout                   100000       649.9        93.6        6.9x
event-succeed             100000      1236.5       142.9        8.7x
callback                  100000        46.9         5.7        8.2x
process-spawn              10000      2839.2      2176.3        1.3x
process-churn             100000      1624.7       166.6        9.8x
resource                  100000      2472.8        67.9       36.4x
priority-resource         100000      3044.0        67.8       44.9x
preemptive-resource       100000      3432.8        95.2       36.1x
container                 100000      2855.2       137.8       20.7x
store                     100000      2767.4       162.1       17.1x
filter-store              100000      2999.5       176.4       17.0x
priority-store            100000      3182.7       164.0       19.4x
allof                     100000      4958.2       242.5       20.4x
anyof                     100000      4696.2       242.1       19.4x
interrupt                 100000      5683.3       547.9       10.4x
---------------------- --------- ----------- ----------- -----------
TOTAL wall (s)                         3.993       0.253       15.8x
```

### Recent optimizations (vs. previous numbers)

| primitive       | before  | after  | what changed |
|-----------------|--------:|-------:|--------------|
| callback        | 30.5 ns | 5.7 ns | event callbacks switched from per-node linked list (`malloc` per subscription) to a packed dynamic array — geometric growth, contiguous iteration |
| timeout         | 108 ns  | 94 ns  | bonus from the same change (`waiter_cb` no longer allocates a node) |
| resource family | ~125 ns | ~70 ns | same bonus across every primitive that subscribes a waiter on yield |
| process-spawn   | 5.1 µs  | 2.2 µs | default stack 64 KiB → 16 KiB (faster `malloc` bin) plus process pool |
| process-churn   | n/a     | 167 ns | new bench — spawn → wait-for-done → respawn. Pool eliminates `malloc(stack)` after warm-up |

### How to read this

- **timeout / event-succeed (~95-145 ns)** — pure heap push/pop + callback dispatch. The min cost of *any* scheduled op.
- **callback (5.7 ns)** — single event, N callbacks, one succeed. The array storage means each subscription is one bounds-check + two stores; no allocator involvement on the hot path.
- **process-spawn (2.2 µs)** — cold spawn-burst (spawn N, then drain). simpyc was slower than SimPy here in earlier builds; with the smaller default stack it now roughly matches. The remaining cost is `malloc(stack) + alloc done event + schedule the initial resume`.
- **process-churn (167 ns)** — the real-world spawn pattern: spawn child, wait for it to die, spawn another. After the first iteration the process pool is populated, so further spawns reuse both the struct and the 16 KiB stack — only event allocation remains. **9.8× faster than SimPy** on this pattern.
- **resource / priority-resource / preemptive-resource (~70-95 ns)** — "yield on a synchronously-succeeded event" plus a list operation. The callback-array change cut this in half.
- **container / store / filter-store / priority-store (~140-175 ns)** — each op is a yield on the producer + a yield on the consumer, so the cost is mostly context-switch + heap work.
- **allof / anyof (~245 ns)** — 4 child timeouts + condition state + 4 waiter callbacks per op.
- **interrupt (550 ns)** — each interrupt schedules an extra resume + swaps into the victim + back out. Two extra context switches per op.

The factory is **13.7× faster** end-to-end. All 14 primitives are now faster than SimPy.

## Knobs

`bench.sh` forwards args. For the factory:

```
--num-workers / --num-shippers / --num-inspectors / --num-reviewers / --num-stations
--num-part-kinds / --sim-time / --material-capacity / --material-initial
--material-per-product / --parts-interval / --assembly-time / --inspect-time
--refill-interval / --refill-amount / --ship-time
--maint-interval / --maint-time / --review-interval / --review-work
--rush-every / --production-target
```

For the micro:

```
-n N            ops per benchmark (default 100k; process-spawn uses N/10)
--only=a,b,c    only run named benchmarks
```

## Layout

```
benchmarking/
├── bench.sh
├── README.md
├── python/
│   ├── factory.py
│   ├── micro.py
│   └── requirements.txt
├── c/
│   ├── factory.c
│   ├── micro.c
│   └── Makefile
└── .venv/        (created on first run)
```
