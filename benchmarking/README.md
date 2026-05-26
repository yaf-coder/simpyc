# benchmarking

Two factory simulators that drive every primitive the C library ports —
one against upstream [SimPy](https://simpy.readthedocs.io), one against
`libsimpyc`. The bench harness runs both, checks they agree on the result,
and tabulates wall time and peak RSS.

## What's exercised

| primitive | simpy                    | simpyc                              |
|-----------|--------------------------|-------------------------------------|
| Environment | `simpy.Environment`, `env.run(until=)` | `sim_env_create`, `sim_run_until`   |
| Timeout   | `env.timeout`            | `sim_timeout`                       |
| Event     | `env.event()`, `.succeed()`, `.callbacks.append` | `sim_event`, `sim_event_succeed`, `sim_event_on` |
| Process   | `env.process(...)`       | `sim_process`, `sim_yield`          |
| AnyOf     | `e1 \| e2`               | `sim_any_of`                        |
| AllOf     | `env.all_of([...])`      | `sim_all_of`                        |
| Resource  | `simpy.Resource(env, N)` | `sim_resource_create`               |
| Container | `simpy.Container`        | `sim_container_create`              |
| Store     | `simpy.Store`            | `sim_store_create`                  |

[`python/factory.py`](python/factory.py) and [`c/factory.c`](c/factory.c)
are line-for-line parallel. Same parameters in → same `produced`,
`shipped`, `target_hit_at` out.

## Run

```
./bench.sh                                       # defaults
./bench.sh --sim-time=200000 --num-workers=16    # bigger
```

First run creates a local `.venv` with simpy 4. Subsequent runs reuse it.

## Results (Apple M-series, `-O3`, single thread)

| workload                              | metric        | simpy(py) | simpyc(c) |  ratio |
|---------------------------------------|---------------|----------:|----------:|-------:|
| sim_time=10k, 4 workers, 2 shippers   | wall (s)      |     0.088 |     0.007 | **12.7× faster** |
|                                       | peak RSS (MB) |     23.4  |     1.5   | **15.8× less**   |
| sim_time=200k, 16 workers, 8 shippers | wall (s)      |     2.97  |     0.26  | **11.4× faster** |
|                                       | peak RSS (MB) |     23.3  |     1.8   | **13.3× less**   |
| sim_time=1M, 32 workers, 16 shippers  | wall (s)      |    15.14  |     1.34  | **11.3× faster** |
|                                       | peak RSS (MB) |     23.7  |     2.1   | **11.1× less**   |

Both implementations agree on `produced`, `shipped`, and `target_hit_at`
for every workload (the bench script `WARN`s if they don't).

## Knobs

`bench.sh` forwards all arguments to both factories. Available:

```
--num-workers=N           default 4
--num-shippers=N          default 2
--sim-time=T              default 10000
--material-capacity=X     default 100
--material-initial=X      default 50
--material-per-product=X  default 2
--assembly-time=T         default 5
--refill-interval=T       default 8
--refill-amount=X         default 20
--ship-time=T             default 3
--production-target=N     default 500
```

## Layout

```
benchmarking/
├── bench.sh
├── README.md
├── python/
│   ├── factory.py
│   └── requirements.txt
├── c/
│   ├── factory.c
│   └── Makefile
└── .venv/        (created on first run)
```
