"""Factory simulator using upstream SimPy.

Mirrors benchmarking/c/factory.c line-for-line so the two can be compared.
Exercises every primitive that simpyc ports:

  Environment   -> simpy.Environment, env.run(until=...)
  Timeout       -> env.timeout
  Event         -> env.event() + .succeed() + .callbacks.append
  Process       -> env.process(generator)
  AnyOf         -> evt | evt
  AllOf         -> env.all_of([...])
  Resource      -> simpy.Resource
  Container     -> simpy.Container (put / get)
  Store         -> simpy.Store (put / get)
"""

import argparse
import resource as urs
import sys
import time

import simpy


DEFAULTS = dict(
    num_workers=4,
    num_shippers=2,
    sim_time=10000.0,
    material_capacity=100.0,
    material_initial=50.0,
    material_per_product=2.0,
    assembly_time=5.0,
    refill_interval=8.0,
    refill_amount=20.0,
    ship_time=3.0,
    production_target=500,
)


class Stats:
    __slots__ = ("produced", "shipped", "target_hit_at")

    def __init__(self):
        self.produced = 0
        self.shipped = 0
        self.target_hit_at = -1.0


def build_factory(env, p, stats):
    materials = simpy.Container(env, p["material_capacity"],
                                init=p["material_initial"])
    workers   = simpy.Resource(env, p["num_workers"])
    store     = simpy.Store(env)
    target    = env.event()

    def on_target(_ev):
        stats.target_hit_at = env.now
    target.callbacks.append(on_target)

    def refiller():
        while True:
            yield env.timeout(p["refill_interval"])
            yield materials.put(p["refill_amount"])

    def worker(name):
        while True:
            with workers.request() as req:
                yield req
                yield materials.get(p["material_per_product"])
                yield env.timeout(p["assembly_time"])
                yield store.put(name)
                stats.produced += 1
                if (stats.produced == p["production_target"]
                        and not target.triggered):
                    target.succeed()

    def shipper(_name):
        while True:
            yield store.get()
            yield env.timeout(p["ship_time"])
            stats.shipped += 1

    def watchdog():
        # AnyOf: production target or a soft half-time deadline.
        deadline = env.timeout(p["sim_time"] * 0.5)
        yield target | deadline
        # AllOf: small bundle to exercise the constructor.
        yield env.all_of([env.timeout(1.0), env.timeout(2.0)])

    env.process(refiller())
    env.process(watchdog())
    for i in range(p["num_workers"]):
        env.process(worker(f"w{i}"))
    for i in range(p["num_shippers"]):
        env.process(shipper(f"s{i}"))


def peak_rss_mb():
    rss = urs.getrusage(urs.RUSAGE_SELF).ru_maxrss
    # macOS reports bytes; Linux reports KB.
    return rss / (1024 * 1024) if sys.platform == "darwin" else rss / 1024


def main():
    ap = argparse.ArgumentParser()
    for k, v in DEFAULTS.items():
        ap.add_argument(f"--{k.replace('_', '-')}",
                        type=type(v), default=v)
    args = ap.parse_args()
    p = {k: getattr(args, k) for k in DEFAULTS}

    env = simpy.Environment()
    stats = Stats()
    build_factory(env, p, stats)

    t0 = time.monotonic()
    env.run(until=p["sim_time"])
    t1 = time.monotonic()

    print(f"impl=simpy")
    print(f"sim_time={env.now:.1f}")
    print(f"produced={stats.produced}")
    print(f"shipped={stats.shipped}")
    print(f"target_hit_at={stats.target_hit_at:.2f}")
    print(f"wall_seconds={t1 - t0:.6f}")
    print(f"peak_rss_mb={peak_rss_mb():.2f}")


if __name__ == "__main__":
    main()
