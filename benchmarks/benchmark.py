#!/usr/bin/env python3
"""Latency and throughput benchmark for the C++ limit order book.

    python3 benchmarks/benchmark.py
    python3 benchmarks/benchmark.py --n 2000000 --repeats 7

Writes a markdown report to benchmarks/results.md.

Two measurement paths are reported, and the distinction matters:

  * **C++ core** -- the work runs entirely inside a native loop with the GIL
    released (bench_driver), so the numbers describe the order book.
  * **Python round-trip** -- the same add driven one call at a time from
    Python. The difference between the two is the cost of the pybind11
    trampoline plus the interpreter, which is worth knowing but is not a
    property of the book.

Single measurements on a laptop are noisy, so each scenario is repeated and
the median is reported.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from typing import Callable, Dict, List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_ROOT, "python"))

try:
    import order_book_cpp as lob
    import bench_driver
except ImportError as exc:  # pragma: no cover - build-time failure path
    sys.exit(
        f"could not import the extension modules ({exc}).\n"
        "Build them first:\n"
        "  cmake -B build -DCMAKE_BUILD_TYPE=Release\n"
        "  cmake --build build --config Release -j 4"
    )

# Scenarios exposed by bench_driver, in report order.
SCENARIOS = [
    ("add", "Passive inserts into a non-crossing book"),
    ("cancel", "Random-order cancels of resting orders (setup excluded)"),
    ("match", "Aggressive orders sweeping the best level, one trade each"),
    ("mixed", "Realistic 70/20/10 add/cancel/aggressive mix"),
]


# ---------------------------------------------------------------------------
# measurement
# ---------------------------------------------------------------------------


def measure(fn: Callable[[], Dict], repeats: int) -> Dict:
    """Run a scenario `repeats` times and summarise with medians."""
    runs = [fn() for _ in range(repeats)]
    ns_per_op = [r["ns_per_op"] for r in runs]
    ops_per_sec = [r["ops_per_sec"] for r in runs]
    return {
        "ops": runs[0]["ops"],
        "trades": runs[0]["trades"],
        "ns_per_op": statistics.median(ns_per_op),
        "ns_per_op_best": min(ns_per_op),
        "ns_per_op_stdev": statistics.stdev(ns_per_op) if len(ns_per_op) > 1 else 0.0,
        "ops_per_sec": statistics.median(ops_per_sec),
        "total_ns": statistics.median([r["total_ns"] for r in runs]),
        "samples": len(runs),
    }


def python_round_trip(n: int, seed: int) -> Dict:
    """Add n orders one Python call at a time, to size the binding overhead."""
    import random

    rng = random.Random(seed)
    book = lob.OrderBook(n)
    params = []
    for _ in range(n):
        side = lob.Side.Bid if rng.random() < 0.5 else lob.Side.Ask
        price = 99.99 - rng.randrange(0, 100) * 0.01 if side == lob.Side.Bid \
            else 100.01 + rng.randrange(0, 100) * 0.01
        params.append((side, price, rng.randint(1, 100)))

    start = time.perf_counter()
    for side, price, qty in params:
        book.add_limit_order(side=side, price=price, quantity=qty)
    elapsed = time.perf_counter() - start

    return {
        "ops": n,
        "ns_per_op": elapsed * 1e9 / n,
        "ops_per_sec": n / elapsed,
        "total_ns": elapsed * 1e9,
    }


# ---------------------------------------------------------------------------
# environment
# ---------------------------------------------------------------------------


def cpu_model() -> str:
    if platform.system() == "Darwin":
        try:
            return subprocess.run(
                ["sysctl", "-n", "machdep.cpu.brand_string"],
                capture_output=True, text=True, timeout=5,
            ).stdout.strip() or platform.processor() or platform.machine()
        except (OSError, subprocess.SubprocessError):
            pass
    return platform.processor() or platform.machine()


def cmake_cache_value(key: str) -> Optional[str]:
    """Read a setting back out of the build that produced these extensions."""
    cache = os.path.join(_ROOT, "build", "CMakeCache.txt")
    if not os.path.exists(cache):
        return None
    with open(cache, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith(key + ":"):
                return line.split("=", 1)[-1].strip()
    return None


def compiler_string() -> str:
    compiler = cmake_cache_value("CMAKE_CXX_COMPILER")
    version = subprocess.run(
        [compiler or "c++", "--version"], capture_output=True, text=True
    ).stdout.splitlines()[0] if (compiler or True) else ""
    return f"{version.strip()}" + (f" ({compiler})" if compiler else "")


def build_type() -> str:
    return cmake_cache_value("CMAKE_BUILD_TYPE") or "unknown"


def pybind11_version() -> str:
    try:
        import pybind11  # noqa: PLC0415 - optional, only needed for the report

        return pybind11.__version__
    except ImportError:
        return "unknown"


def extension_version() -> str:
    return getattr(bench_driver, "__version__", "unknown")


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------


def render_markdown(results: Dict, meta: Dict, py_rt: Dict) -> str:
    n = results["n"]
    lines: List[str] = []

    lines.append("# Benchmark results")
    lines.append("")
    lines.append("Generated by `benchmarks/benchmark.py`. "
                 "Reproduce with `python3 benchmarks/benchmark.py`.")
    lines.append("")
    lines.append("| | |")
    lines.append("|---|---|")
    lines.append(f"| Date | {meta['date']} |")
    lines.append(f"| Machine | {meta['machine']} |")
    lines.append(f"| CPU | {meta['cpu']} |")
    lines.append(f"| Compiler | {meta['compiler']} |")
    lines.append(f"| Build type | {meta['build_type']} |")
    lines.append(f"| Python | {meta['python']} |")
    lines.append(f"| pybind11 | {meta['pybind11']} |")
    lines.append(f"| Extension | order_book_cpp / bench_driver {meta['extension']} |")
    lines.append(f"| Workload | {n:,} operations per scenario, "
                 f"{meta['repeats']} repetitions, median reported |")
    lines.append(f"| Seed | {meta['seed']} |")
    lines.append("")

    lines.append("## C++ core")
    lines.append("")
    lines.append("Measured inside a native loop with the GIL released, so these "
                 "describe the order book itself. Timestamps are supplied by the "
                 "harness rather than read from the clock, matching how a feed "
                 "handler stamps orders.")
    lines.append("")
    lines.append("| Scenario | Operations | Avg latency (ns/op) | Best (ns/op) | "
                 "Throughput (ops/sec) | Trades |")
    lines.append("|---|---:|---:|---:|---:|---:|")
    for key, _description in SCENARIOS:
        s = results[key]
        lines.append(
            f"| `{key}` | {s['ops']:,} | {s['ns_per_op']:.1f} | {s['ns_per_op_best']:.1f} | "
            f"{s['ops_per_sec']:,.0f} | {s['trades']:,} |"
        )
    clock = results["clock"]
    lines.append(f"| `clock` (1M `now_ns()`) | {clock['ops']:,} | {clock['ns_per_op']:.1f} | "
                 f"{clock['ns_per_op_best']:.1f} | {clock['ops_per_sec']:,.0f} | - |")
    lines.append("")
    lines.append("Scenario definitions:")
    lines.append("")
    for key, description in SCENARIOS:
        lines.append(f"- **`{key}`** — {description}")
    lines.append(f"- **`clock`** — cost of one `now_ns()` call. The book does not pay this "
                 f"per order because the caller supplies feed timestamps; adding "
                 f"~{clock['ns_per_op']:.0f} ns to every insert is what that design avoids.")
    lines.append("")

    lines.append("## Python round-trip")
    lines.append("")
    lines.append("The same insert driven one call at a time from Python, for comparison:")
    lines.append("")
    lines.append("| Path | Operations | Avg latency (ns/op) | Throughput (ops/sec) |")
    lines.append("|---|---:|---:|---:|")
    lines.append(f"| `add` from C++ (GIL released) | {results['add']['ops']:,} | "
                 f"{results['add']['ns_per_op']:.1f} | {results['add']['ops_per_sec']:,.0f} |")
    lines.append(f"| `add` from Python (per-call) | {py_rt['ops']:,} | "
                 f"{py_rt['ns_per_op']:.1f} | {py_rt['ops_per_sec']:,.0f} |")
    lines.append("")
    overhead = py_rt["ns_per_op"] - results["add"]["ns_per_op"]
    ratio = py_rt["ns_per_op"] / results["add"]["ns_per_op"] if results["add"]["ns_per_op"] else 0
    lines.append(f"Crossing the binding costs roughly **{overhead:,.0f} ns per call** "
                 f"({ratio:.1f}x the C++-only path). That overhead is the interpreter and "
                 f"the pybind11 trampoline, not the order book — which is why the "
                 f"throughput-critical paths live in C++ and Python drives them in bulk.")
    lines.append("")

    lines.append("## Notes")
    lines.append("")
    lines.append("- Absolute numbers depend heavily on the machine, the compiler and the "
                 "CPU governor. Treat them as orders of magnitude, not as constants.")
    lines.append("- `cancel` is the most cache-hostile scenario: 1,000,000 resting orders "
                 "are cancelled in random order, so nearly every unlink touches cold "
                 "memory. A book with a hot working set cancels considerably faster.")
    lines.append("- `-march=native` is enabled in Release, so these numbers are tuned for "
                 "the build machine.")
    lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--n", type=int, default=1_000_000,
                        help="operations per scenario (default: 1000000)")
    parser.add_argument("--repeats", type=int, default=5,
                        help="repetitions per scenario; the median is reported")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--out", default=os.path.join(_HERE, "results.md"))
    parser.add_argument("--python-n", type=int, default=200_000,
                        help="operations for the Python round-trip measurement")
    args = parser.parse_args()

    if args.repeats < 1:
        parser.error("--repeats must be at least 1")

    print(f"C++ Limit Order Book benchmark -- n={args.n:,}, "
          f"{args.repeats} repetitions per scenario\n")

    results: Dict = {"n": args.n, "seed": args.seed}

    for key, description in SCENARIOS:
        print(f"  running {key:<8} {description} ...", end="", flush=True)
        stats = measure(lambda k=key: getattr(bench_driver, f"bench_{k}")(n=args.n, seed=args.seed),
                        args.repeats)
        results[key] = stats
        print(f" {stats['ns_per_op']:.1f} ns/op  ({stats['ops_per_sec']:,.0f} ops/sec)")

    clock = measure(lambda: bench_driver.bench_clock(n=args.n, seed=args.seed), args.repeats)
    results["clock"] = clock
    print(f"  running {'clock':<8} now_ns() cost ... {clock['ns_per_op']:.1f} ns/op")

    print(f"\n  running Python round-trip at n={args.python_n:,} ...", end="", flush=True)
    py_rt = python_round_trip(args.python_n, args.seed)
    print(f" {py_rt['ns_per_op']:.1f} ns/op")

    meta = {
        "date": _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "machine": f"{platform.system()} {platform.release()} ({platform.machine()})",
        "cpu": cpu_model(),
        "compiler": compiler_string(),
        "build_type": build_type(),
        "python": platform.python_version(),
        "pybind11": pybind11_version(),
        "extension": extension_version(),
        "repeats": args.repeats,
        "seed": args.seed,
    }

    report = render_markdown(results, meta, py_rt)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(report)

    print(f"\nWrote {args.out}")
    print("\nSummary")
    print(f"  add     {results['add']['ns_per_op']:8.1f} ns/op   "
          f"{results['add']['ops_per_sec']:>14,.0f} ops/sec")
    print(f"  cancel  {results['cancel']['ns_per_op']:8.1f} ns/op   "
          f"{results['cancel']['ops_per_sec']:>14,.0f} ops/sec")
    print(f"  match   {results['match']['ns_per_op']:8.1f} ns/op   "
          f"{results['match']['ops_per_sec']:>14,.0f} ops/sec")
    print(f"  mixed   {results['mixed']['ns_per_op']:8.1f} ns/op   "
          f"{results['mixed']['ops_per_sec']:>14,.0f} ops/sec")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
