"""Factory simulator using upstream SimPy. Exercises EVERY primitive that
simpyc ports, in a deterministic way that can be matched by the C version.

  Environment / Timeout / Event(succeed + callback) / Process
  AnyOf / AllOf
  Resource (FIFO)        — stations
  PriorityResource       — inspectors (rush items get priority)
  PreemptiveResource     — shared machine; maintenance preempts workers
  Container              — raw materials
  Store                  — finished-products buffer
  FilterStore            — parts bin (workers pull matching kinds)
  PriorityStore          — shipping queue (rush items shipped first)
  Process.interrupt      — supervisor periodically reviews idle reviewers
"""

import argparse
import resource as urs
import sys
import time

import simpy


DEFAULTS = dict(
    num_workers=4,
    num_inspectors=2,
    num_reviewers=2,
    num_stations=4,
    num_part_kinds=2,
    sim_time=10000.0,
    material_capacity=200.0,
    material_initial=100.0,
    material_per_product=2.0,
    parts_interval=2.0,
    assembly_time=5.0,
    inspect_time=2.0,
    refill_interval=8.0,
    refill_amount=20.0,
    ship_time=3.0,
    maint_interval=50.0,
    maint_time=4.0,
    review_interval=30.0,
    review_work=20.0,
    rush_every=5,                # every 5th product is rush
    production_target=500,
)


class Stats:
    __slots__ = ("produced", "shipped", "preempted", "reviewed",
                 "target_hit_at")
    def __init__(self):
        self.produced = 0
        self.shipped = 0
        self.preempted = 0
        self.reviewed = 0
        self.target_hit_at = -1.0


def build_factory(env, p, stats):
    materials  = simpy.Container(env, p["material_capacity"],
                                 init=p["material_initial"])
    parts      = simpy.FilterStore(env)
    stations   = simpy.Resource(env, p["num_stations"])
    machine    = simpy.PreemptiveResource(env, 1)
    inspectors = simpy.PriorityResource(env, p["num_inspectors"])
    ship_q     = simpy.PriorityStore(env)
    finished   = simpy.Store(env)
    target     = env.event()

    def on_target(_ev):
        stats.target_hit_at = env.now
    target.callbacks.append(on_target)

    def refiller():
        while True:
            yield env.timeout(p["refill_interval"])
            yield materials.put(p["refill_amount"])

    def parts_supplier():
        n = 0
        while True:
            yield env.timeout(p["parts_interval"])
            kind = n % p["num_part_kinds"]
            yield parts.put({"kind": kind, "id": n})
            n += 1

    def maintenance():
        while True:
            yield env.timeout(p["maint_interval"])
            with machine.request(priority=0, preempt=True) as req:
                yield req
                yield env.timeout(p["maint_time"])

    def worker(idx):
        my_kind = idx % p["num_part_kinds"]
        while True:
            yield materials.get(p["material_per_product"])
            yield parts.get(lambda x, k=my_kind: x["kind"] == k)
            with stations.request() as sreq:
                yield sreq
                preempted = False
                with machine.request(priority=10, preempt=True) as mreq:
                    yield mreq
                    try:
                        yield env.timeout(p["assembly_time"])
                    except simpy.Interrupt:
                        preempted = True
                        stats.preempted += 1
                if not preempted:
                    rush = (stats.produced % p["rush_every"] == 0)
                    prio = 0 if rush else 5
                    with inspectors.request(priority=prio) as ireq:
                        yield ireq
                        yield env.timeout(p["inspect_time"])
                    yield finished.put({"id": stats.produced,
                                        "priority": prio})
                    stats.produced += 1
                    if (stats.produced == p["production_target"]
                            and not target.triggered):
                        target.succeed()

    def aggregator():
        while True:
            item = yield finished.get()
            yield ship_q.put(simpy.PriorityItem(item["priority"], item))

    def dispatcher():
        while True:
            yield ship_q.get()
            yield env.timeout(p["ship_time"])
            stats.shipped += 1

    def reviewer():
        while True:
            try:
                yield env.timeout(p["review_work"])
            except simpy.Interrupt:
                stats.reviewed += 1

    reviewer_procs = [env.process(reviewer()) for _ in range(p["num_reviewers"])]

    def supervisor():
        while True:
            yield env.timeout(p["review_interval"])
            for rp in reviewer_procs:
                if rp.is_alive:
                    rp.interrupt("review")

    def watchdog():
        deadline = env.timeout(p["sim_time"] * 0.5)
        yield target | deadline
        yield env.all_of([env.timeout(1.0), env.timeout(2.0)])

    env.process(refiller())
    env.process(parts_supplier())
    env.process(maintenance())
    env.process(supervisor())
    env.process(aggregator())
    env.process(dispatcher())
    env.process(watchdog())
    for i in range(p["num_workers"]):
        env.process(worker(i))


def peak_rss_mb():
    rss = urs.getrusage(urs.RUSAGE_SELF).ru_maxrss
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
    print(f"preempted={stats.preempted}")
    print(f"reviewed={stats.reviewed}")
    print(f"target_hit_at={stats.target_hit_at:.2f}")
    print(f"wall_seconds={t1 - t0:.6f}")
    print(f"peak_rss_mb={peak_rss_mb():.2f}")


if __name__ == "__main__":
    main()
