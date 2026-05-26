"""Per-primitive micro-benchmarks for SimPy.

Each bench runs a tight loop of N operations of a single primitive and
prints `bench=<name> ops=<n> wall_seconds=<t> ns_per_op=<x>` so the
shell harness can pair lines with the C output. Mirrors c/micro.c.
"""
import argparse
import gc
import resource as urs
import sys
import time

import simpy


# -------------------------------------------------------------------- timers

def measure(name, fn, n):
    gc.collect()
    t0 = time.monotonic()
    fn(n)
    dt = time.monotonic() - t0
    print(f"bench={name} ops={n} wall_seconds={dt:.6f} "
          f"ns_per_op={dt*1e9/n:.1f}", flush=True)


# -------------------------------------------------------------------- benches

def b_timeout(n):
    env = simpy.Environment()
    def p():
        for _ in range(n):
            yield env.timeout(1.0)
    env.process(p())
    env.run()

def b_event_succeed(n):
    env = simpy.Environment()
    for _ in range(n):
        e = env.event()
        e.succeed()
    env.run()

def b_callback(n):
    """Single event; N callbacks all fire on succeed."""
    env = simpy.Environment()
    e = env.event()
    def cb(_ev): pass
    for _ in range(n):
        e.callbacks.append(cb)
    e.succeed()
    env.run()

def b_process_spawn(n):
    env = simpy.Environment()
    def noop():
        if False: yield        # make it a generator that returns immediately
    for _ in range(n):
        env.process(noop())
    env.run()

def b_process_churn(n):
    """Spawn-wait-die-respawn N times. Mirrors the C process-churn bench."""
    env = simpy.Environment()
    def noop():
        if False: yield
    def spawner():
        for _ in range(n):
            yield env.process(noop())
    env.process(spawner())
    env.run()

def b_resource(n):
    env = simpy.Environment()
    r = simpy.Resource(env, 1)
    def p():
        for _ in range(n):
            req = r.request()
            yield req
            r.release(req)
    env.process(p())
    env.run()

def b_priority_resource(n):
    env = simpy.Environment()
    r = simpy.PriorityResource(env, 1)
    def p():
        for _ in range(n):
            req = r.request(priority=0)
            yield req
            r.release(req)
    env.process(p())
    env.run()

def b_preemptive_resource(n):
    env = simpy.Environment()
    r = simpy.PreemptiveResource(env, 1)
    def p():
        for _ in range(n):
            req = r.request(priority=0, preempt=False)
            yield req
            r.release(req)
    env.process(p())
    env.run()

def b_container(n):
    env = simpy.Environment()
    c = simpy.Container(env, capacity=n * 2, init=0)
    def producer():
        for _ in range(n):
            yield c.put(1)
    def consumer():
        for _ in range(n):
            yield c.get(1)
    env.process(producer())
    env.process(consumer())
    env.run()

def b_store(n):
    env = simpy.Environment()
    s = simpy.Store(env)
    def producer():
        for i in range(n):
            yield s.put(i)
    def consumer():
        for _ in range(n):
            yield s.get()
    env.process(producer())
    env.process(consumer())
    env.run()

def b_filter_store(n):
    env = simpy.Environment()
    s = simpy.FilterStore(env)
    accept_all = lambda _x: True
    def producer():
        for i in range(n):
            yield s.put(i)
    def consumer():
        for _ in range(n):
            yield s.get(accept_all)
    env.process(producer())
    env.process(consumer())
    env.run()

def b_priority_store(n):
    env = simpy.Environment()
    s = simpy.PriorityStore(env)
    def producer():
        for i in range(n):
            # encode i in priority so heapq doesn't compare items
            yield s.put(simpy.PriorityItem(i, i))
    def consumer():
        for _ in range(n):
            yield s.get()
    env.process(producer())
    env.process(consumer())
    env.run()

def b_allof(n):
    """N AllOf conditions, each over 4 inputs."""
    env = simpy.Environment()
    def p():
        for _ in range(n):
            evs = [env.timeout(0) for _ in range(4)]
            yield env.all_of(evs)
    env.process(p())
    env.run()

def b_anyof(n):
    env = simpy.Environment()
    def p():
        for _ in range(n):
            evs = [env.timeout(0) for _ in range(4)]
            yield env.any_of(evs)
    env.process(p())
    env.run()

def b_interrupt(n):
    """One target process suspends; another interrupts it N times."""
    env = simpy.Environment()
    def victim():
        cnt = 0
        while cnt < n:
            try:
                yield env.timeout(1e9)
            except simpy.Interrupt:
                cnt += 1
    v = env.process(victim())
    def driver():
        for _ in range(n):
            yield env.timeout(0)
            v.interrupt("x")
    env.process(driver())
    env.run()


# (name, fn, n_scale) — scale is applied to the user's --n; some benches
# get fewer iterations because each op allocates non-trivial state
# (e.g. process-spawn keeps a stack per process).
BENCHES = [
    ("timeout",             b_timeout,             1.0),
    ("event-succeed",       b_event_succeed,       1.0),
    ("callback",            b_callback,            1.0),
    ("process-spawn",       b_process_spawn,       0.1),
    ("process-churn",       b_process_churn,       1.0),
    ("resource",            b_resource,            1.0),
    ("priority-resource",   b_priority_resource,   1.0),
    ("preemptive-resource", b_preemptive_resource, 1.0),
    ("container",           b_container,           1.0),
    ("store",               b_store,               1.0),
    ("filter-store",        b_filter_store,        1.0),
    ("priority-store",      b_priority_store,      1.0),
    ("allof",               b_allof,               1.0),
    ("anyof",               b_anyof,               1.0),
    ("interrupt",           b_interrupt,           1.0),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=100000,
                    help="ops per benchmark (default 100k)")
    ap.add_argument("--only", help="comma-separated bench names")
    args = ap.parse_args()
    want = set(args.only.split(",")) if args.only else None

    print(f"impl=simpy n={args.n}")
    for name, fn, scale in BENCHES:
        if want and name not in want: continue
        n_use = max(1000, int(args.n * scale))
        measure(name, fn, n_use)

    rss = urs.getrusage(urs.RUSAGE_SELF).ru_maxrss
    rss_mb = rss / (1024 * 1024) if sys.platform == "darwin" else rss / 1024
    print(f"peak_rss_mb={rss_mb:.2f}")


if __name__ == "__main__":
    main()
