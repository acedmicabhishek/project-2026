"""
Matching-engine benchmark: C++ (quantsim_core) vs pure-Python gateway.

Measures the hot path — match one marketable order against the top of book —
repeated N times, and reports throughput + latency percentiles for each engine.
This is the "true low-latency" proof: the same matching work, compiled C++ vs
interpreted Python, including the pybind11 marshaling cost (so it is the *real*
cost of calling C++ from Python, not a rigged microbench).

    PYTHONPATH=python python3 examples/bench_matching.py --n 200000

Both engines match a small buy against a large resting sell at the touch, so each
iteration does exactly one match with no per-iteration book rebuild.
"""

from __future__ import annotations

import argparse
import statistics
from time import perf_counter_ns

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))

from quantsim.live.feed    import LiveBook, BookLevel
from quantsim.live.gateway import PaperGateway

try:
    import quantsim_core as qc
    HAVE_CPP = True
except ImportError:
    HAVE_CPP = False


PRICE = 100.0
SCALE = 1_000_000          # micro-lots: fractional qty -> integer for C++


def pct(samples, p):
    if not samples:
        return 0.0
    k = max(0, min(len(samples) - 1, int(round(p / 100.0 * (len(samples) - 1)))))
    return sorted(samples)[k]


def bench_python(n: int, sample_every: int):
    gw = PaperGateway(taker_fee_bps=0)
    fills = []
    gw.on_fill = fills.append
    gw.on_book(LiveBook("X", 0, [BookLevel(PRICE - 0.01, 1e9)],
                        [BookLevel(PRICE, 1e9)]))   # huge resting liquidity
    lat = []
    t0 = perf_counter_ns()
    for i in range(n):
        fills.clear()
        s = perf_counter_ns()
        gw.submit(i + 1, "X", "B", 0.001, 0.0, "MARKET")
        if i % sample_every == 0:
            lat.append(perf_counter_ns() - s)
    total = perf_counter_ns() - t0
    return total, lat


def bench_cpp(n: int, sample_every: int):
    eng = qc.MatchingEngine()
    # one huge resting sell at the touch; buys match against it forever
    rest = qc.Order()
    rest.id = 1
    rest.symbol = "X"
    rest.side = qc.Side.Sell
    rest.type = qc.OrdType.Limit
    rest.price = PRICE
    rest.qty = 2 * n * 1000 + 1000     # never fully consumed
    eng.submit(rest)

    lot = max(1, int(0.001 * SCALE))
    lat = []
    oid = 1
    t0 = perf_counter_ns()
    for i in range(n):
        s = perf_counter_ns()
        o = qc.Order()
        oid += 1
        o.id = oid
        o.symbol = "X"
        o.side = qc.Side.Buy
        o.type = qc.OrdType.Market
        o.price = 0.0
        o.qty = lot
        eng.submit(o)
        if i % sample_every == 0:
            lat.append(perf_counter_ns() - s)
    total = perf_counter_ns() - t0
    return total, lat


def bench_cpp_batch(n: int):
    """The real C++ hot path: one pybind crossing, N matches looped in C++."""
    eng = qc.MatchingEngine()
    rest = qc.Order()
    rest.id = 1; rest.symbol = "X"; rest.side = qc.Side.Sell
    rest.type = qc.OrdType.Limit; rest.price = PRICE
    rest.qty = 2 * n * 1000 + 1000
    eng.submit(rest)

    lot   = max(1, int(0.001 * SCALE))
    sides = [0] * n               # all Buy
    types = [1] * n               # all Market
    prices = [0.0] * n
    qtys  = [lot] * n
    t0 = perf_counter_ns()
    res = eng.submit_batch("X", sides, types, prices, qtys)
    total = perf_counter_ns() - t0
    return total, res["fills"]


def report(name: str, total_ns: int, lat: list, n: int):
    ops = n / (total_ns / 1e9)
    print(f"  {name:<8}  {ops:>12,.0f} ops/s   "
          f"mean {total_ns/n:>7.0f} ns   "
          f"p50 {pct(lat,50):>6.0f}   p99 {pct(lat,99):>7.0f}   "
          f"p99.9 {pct(lat,99.9):>8.0f} ns")
    return ops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200_000)
    ap.add_argument("--sample-every", type=int, default=5)
    args = ap.parse_args()
    n = args.n

    print(f"\nMatching benchmark — {n:,} matches/engine\n" + "─" * 78)

    # warm up
    bench_python(2000, 1)
    tp, lp = bench_python(n, args.sample_every)
    py_ops = report("Python", tp, lp, n)

    if HAVE_CPP:
        bench_cpp(2000, 1)
        tc, lc = bench_cpp(n, args.sample_every)
        cpp_ops = report("C++/call", tc, lc, n)

        bench_cpp_batch(2000)
        tb, fills = bench_cpp_batch(n)
        batch_ops = n / (tb / 1e9)
        print(f"  {'C++/batch':<8}  {batch_ops:>12,.0f} ops/s   "
              f"mean {tb/n:>7.0f} ns   (one pybind call, {fills:,} fills)")

        print("─" * 78)
        print(f"  per-order C++ via pybind: {cpp_ops/py_ops:>4.1f}× vs Python "
              f"(boundary cost dominates — slower)")
        print(f"  batched  C++ (loop in C++): {batch_ops/py_ops:>4.1f}× vs Python "
              f"— this is the real low-latency path")
    else:
        print("  C++  quantsim_core not importable — build it (./start.sh) to compare")
    print()


if __name__ == "__main__":
    main()

