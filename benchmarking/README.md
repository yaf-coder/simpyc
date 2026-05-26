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
timeout                   100000       645.5       108.3        6.0x
event-succeed             100000      1444.6       124.0       11.6x
callback                  100000        56.1        30.5        1.8x
process-spawn              10000      2983.7      5105.1        0.6x
resource                  100000      2449.0       124.0       19.8x
priority-resource         100000      3125.4       120.9       25.9x
preemptive-resource       100000      3552.5       152.3       23.3x
container                 100000      3024.9       278.0       10.9x
store                     100000      2764.4       302.9        9.1x
filter-store              100000      3074.1       300.8       10.2x
priority-store            100000      3211.9       293.7       10.9x
allof                     100000      5051.5       366.0       13.8x
anyof                     100000      4945.2       365.4       13.5x
interrupt                 100000      5926.7       642.8        9.2x
---------------------- --------- ----------- ----------- -----------
TOTAL wall (s)                         3.957       0.372       10.6x
```

### How to read this

- **timeout / event-succeed (~110-130 ns)** — pure heap push/pop + callback dispatch. The min cost of *any* scheduled op in simpyc.
- **callback (30 ns)** — single event, N callbacks, one succeed. Both implementations are basically just appending to a list, so the gap closes. Python's overhead is the function-call dispatch in the loop.
- **process-spawn (5.1 µs in simpyc — slower than SimPy)** — the one place simpyc loses. SimPy's `process()` allocates a Python generator, which is a few hundred bytes. simpyc allocates a 64 KiB stack (`malloc`) per process so the coroutine has somewhere to run. For workloads that spawn many short-lived processes this matters; for workloads with a stable set of long-lived processes (the SimPy norm) it doesn't. Reducing `SIM_DEFAULT_STACK` in `src/process.c` shrinks this immediately at the cost of recursion headroom.
- **resource / priority-resource / preemptive-resource (~120-150 ns)** — these are essentially "yield on a synchronously-succeeded event" plus a list operation. Even preemptive (which searches the holders list to find a victim) is fast because the holder list is empty/small in the no-contention bench.
- **container / store / filter-store / priority-store (~280-300 ns)** — each op is two yields (put + get) split across two processes, so the per-op cost is dominated by context-switch + heap work, not the data structure. They all land in the same band because the data-structure work is small relative to the simulator overhead.
- **allof / anyof (~365 ns)** — 4 events scheduled + 4 waiter callbacks + 1 condition resolution per op. The extra cost vs `timeout` is the four child timeouts and the condition state.
- **interrupt (640 ns)** — each interrupt schedules a resume, swaps into the victim, swaps back out. About 2× a plain yield. Roughly matches Python's relative overhead (also the most expensive primitive there).

The factory is **13.7× faster** end-to-end even though `process-spawn` is the one primitive simpyc is slower at — because the factory has a fixed set of long-lived processes, the spawn cost is paid once and amortized across millions of yields.

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
