#!/usr/bin/env python3
"""Command line driver for the C++ limit order book.

    python3 cli.py demo
    python3 cli.py stress --n 1000000
    python3 cli.py scenario --file ../scenarios/example.json

The heavy lifting happens in C++. This script only feeds the book and renders
the result.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import sys
import time
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

# The pybind11 extension is copied next to this file by the CMake build, and
# Python only puts the *script's* directory on sys.path -- which is not the same
# thing when cli.py is invoked through a symlink or from another cwd.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import order_book_cpp as lob
except ImportError as exc:  # pragma: no cover - build-time failure path
    sys.exit(
        f"could not import order_book_cpp ({exc}).\n"
        "Build it first:\n"
        "  cmake -B build -DCMAKE_BUILD_TYPE=Release\n"
        "  cmake --build build --config Release -j 4"
    )

try:
    import bench_driver
except ImportError:  # pragma: no cover - optional
    bench_driver = None


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def fmt_price(value: float, width: int = 9) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "-".rjust(width)
    return f"{value:.2f}".rjust(width)


def render_book(book: "lob.OrderBook", depth: int = 6) -> str:
    """Render a depth ladder with the spread in the middle."""
    asks = book.top_n_asks(depth)
    bids = book.top_n_bids(depth)

    if not asks and not bids:
        return "  (empty book)"

    lines: List[str] = []
    for price, qty in reversed(asks):  # worst ask at the top, like a real ladder
        lines.append(f"   ASK  {fmt_price(price)}  x {qty:>8}")

    if asks and bids:
        lines.append(
            f"  {'-' * 34}  spread {book.spread():.2f}   mid {fmt_price(book.mid_price()).strip()}"
        )
    else:
        lines.append(f"  {'-' * 34}  one-sided")

    for price, qty in bids:
        lines.append(f"   BID  {fmt_price(price)}  x {qty:>8}")

    return "\n".join(lines)


def render_trades(trades: Sequence["lob.Trade"]) -> str:
    if not trades:
        return "  (no trades)"
    return "\n".join(
        f"  #{t.buy_order_id} (buy) x #{t.sell_order_id} (sell)  "
        f"{t.quantity:>6} @ {t.price:.2f}"
        for t in trades
    )


def banner(title: str) -> None:
    print()
    print(f"=== {title} " + "=" * max(0, 62 - len(title)))


def print_book(book: "lob.OrderBook", depth: int = 6) -> None:
    print(render_book(book, depth))
    print(f"  {book}")


# ---------------------------------------------------------------------------
# demo
# ---------------------------------------------------------------------------


def cmd_demo(args: argparse.Namespace) -> int:
    book = lob.OrderBook()
    depth = args.depth

    banner("1. Build a book: bids below 100, asks above")
    script: Iterable[Tuple[str, float, int]] = [
        ("bid", 99.90, 300),
        ("bid", 99.80, 500),
        ("bid", 99.80, 200),
        ("bid", 99.70, 400),
        ("ask", 100.10, 250),
        ("ask", 100.20, 600),
        ("ask", 100.30, 150),
    ]
    for side, price, qty in script:
        side_enum = lob.Side.Bid if side == "bid" else lob.Side.Ask
        order_id = book.add_limit_order(side=side_enum, price=price, quantity=qty)
        print(f"  add {side:>3} {price:>7.2f} x {qty:>4}  -> order #{order_id}")
    book.match()  # nothing crosses; clears the buffer
    print()
    print_book(book, depth)

    banner("2. Aggressive sell sweeps the bid: partial fill")
    # One order for 450 sits against 800 of bid liquidity at the top two levels.
    order_id = book.add_limit_order(side=lob.Side.Ask, price=99.60, quantity=450)
    trades = book.match()
    print(f"  add ask  99.60 x 450  -> order #{order_id} (crosses the spread)")
    print()
    print(render_trades(trades))
    print()
    print("  traded through the 99.90 level, then partially through 99.80:")
    print()
    print_book(book, depth)

    banner("3. modify: reduce keeps priority, increase re-queues")
    first, second = 2, 3  # the two orders resting at 99.80
    print(f"  queue at 99.80, front first : {_queue_at(book, lob.Side.Bid, 99.80)}")
    book.modify_order(order_id=first, new_quantity=100)
    print(f"  modify #{first} 500 -> 100 (reduction): {_queue_at(book, lob.Side.Bid, 99.80)}"
          "   <- priority kept")
    book.modify_order(order_id=first, new_quantity=900)
    print(f"  modify #{first} 100 -> 900 (increase):  {_queue_at(book, lob.Side.Bid, 99.80)}"
          "   <- re-queued behind #3")

    banner("4. cancel")
    print(f"  cancel #{second} (resting at 99.80): {book.cancel_order(order_id=second)}")
    print(f"  cancel #9999 (never existed):     {book.cancel_order(order_id=9999)}")
    print()
    print_book(book, depth)

    banner("5. Final state")
    print_book(book, depth)
    print()
    print(f"  trades executed : {book.trades_executed()}")
    print(f"  volume traded   : {book.volume_traded()}")
    print(f"  last trade price: {book.last_trade_price():.2f}")
    print(f"  validate()      : {book.validate() or 'consistent'}")
    return 0


def _queue_at(book: "lob.OrderBook", side: Any, price: float) -> List[int]:
    """Order ids resting at one price, front of the queue first."""
    return [o.id for o in book.level_orders(side=side, price=price)]


# ---------------------------------------------------------------------------
# stress
# ---------------------------------------------------------------------------


def cmd_stress(args: argparse.Namespace) -> int:
    n = args.n
    seed = args.seed

    if bench_driver is None:
        print("bench_driver is not built; run the CMake build to enable stress.")
        return 1

    banner(f"Native throughput (C++, GIL released)  n={n:,}")

    suite = bench_driver.bench_suite(n=n, seed=seed)
    header = f"  {'scenario':<10}{'ns/op':>10}{'ops/sec':>14}{'trades':>12}"
    print(header)
    print("  " + "-" * (len(header) - 2))
    for name in ("add", "cancel", "match", "mixed"):
        stats = suite[name]
        print(
            f"  {name:<10}{stats['ns_per_op']:>10.1f}"
            f"{stats['ops_per_sec']:>14,.0f}{stats['trades']:>12,}"
        )
    clock = suite["clock"]
    print(f"\n  (a single now_ns() call costs {clock['ns_per_op']:.1f} ns, which is why the")
    print("   harness passes feed timestamps instead of reading the clock per order)")

    # ---- correctness pass through the Python API -------------------------
    m = args.verify_n
    banner(f"Correctness + Python-API throughput  n={m:,}")
    book = lob.OrderBook(m)
    rng = random.Random(seed)
    ids: List[int] = []

    start = time.perf_counter()
    for i in range(m):
        side = lob.Side.Bid if rng.random() < 0.5 else lob.Side.Ask
        # A band that never crosses, so this measures pure insert.
        price = round(100.0 - rng.uniform(0.01, 5.0), 2) if side == lob.Side.Bid \
            else round(100.0 + rng.uniform(0.01, 5.0), 2)
        order_id = book.add_limit_order(side=side, price=price, quantity=rng.randint(1, 500))
        ids.append(order_id)
    elapsed = time.perf_counter() - start
    print(f"  inserted {m:,} orders in {elapsed:.3f}s "
          f"-> {m / elapsed:,.0f} orders/sec (includes Python call overhead)")
    print(f"  best_bid={fmt_price(book.best_bid()).strip()}  "
          f"best_ask={fmt_price(book.best_ask()).strip()}  "
          f"levels={book.bid_levels()}/{book.ask_levels()}")

    rng.shuffle(ids)
    start = time.perf_counter()
    cancelled = sum(1 for order_id in ids if book.cancel_order(order_id=order_id))
    elapsed = time.perf_counter() - start
    print(f"  cancelled {cancelled:,} orders in {elapsed:.3f}s "
          f"-> {cancelled / elapsed:,.0f} cancels/sec")

    problem = book.validate()
    print(f"  invariants      : {problem or 'consistent'}")
    if book.total_orders() != 0:
        print(f"  ERROR: {book.total_orders()} orders still resting after cancelling all")
        return 1
    return 0


# ---------------------------------------------------------------------------
# scenario
# ---------------------------------------------------------------------------


def cmd_scenario(args: argparse.Namespace) -> int:
    with open(args.file, "r", encoding="utf-8") as handle:
        scenario = json.load(handle)

    if isinstance(scenario, dict):
        operations = scenario.get("operations", [])
    else:
        operations = scenario

    book = lob.OrderBook()
    depth = args.depth
    applied = 0

    for step, op in enumerate(operations, start=1):
        kind = str(op.get("op", "")).lower()

        if kind == "add":
            side_name = str(op["side"]).lower()
            if side_name not in ("bid", "ask"):
                print(f"  step {step}: unknown side {op['side']!r}, skipped")
                continue
            side = lob.Side.Bid if side_name == "bid" else lob.Side.Ask
            order_id = book.add_limit_order(
                side=side,
                price=float(op["price"]),
                quantity=int(op["qty"]),
            )
            trades = book.match()
            print(f"  step {step}: add {side_name:>3} {float(op['price']):>8.2f} "
                  f"x {int(op['qty']):>5} -> #{order_id}")
            for trade in trades:
                print(f"            fill: #{trade.buy_order_id} x #{trade.sell_order_id} "
                      f"{trade.quantity} @ {trade.price:.2f}")

        elif kind == "cancel":
            ok = book.cancel_order(order_id=int(op["id"]))
            print(f"  step {step}: cancel #{int(op['id'])} -> {ok}")

        elif kind == "modify":
            ok = book.modify_order(order_id=int(op["id"]), new_quantity=int(op["qty"]))
            print(f"  step {step}: modify #{int(op['id'])} -> qty {int(op['qty'])} -> {ok}")

        elif kind == "match":
            trades = book.match()
            print(f"  step {step}: match -> {len(trades)} trade(s)")
            for trade in trades:
                print(f"            fill: #{trade.buy_order_id} x #{trade.sell_order_id} "
                      f"{trade.quantity} @ {trade.price:.2f}")

        elif kind == "depth":
            print(f"  step {step}: depth")
            print(render_book(book, int(op.get("n", depth))))

        else:
            print(f"  step {step}: unknown op {kind!r}, skipped")
            continue
        applied += 1

    banner("Final book")
    print_book(book, depth)
    print()
    print(f"  operations applied : {applied}/{len(operations)}")
    print(f"  trades executed    : {book.trades_executed()}")
    print(f"  volume traded      : {book.volume_traded()}")
    print(f"  validate()         : {book.validate() or 'consistent'}")
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cli.py",
        description="Drive the C++ limit order book from Python.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    demo = sub.add_parser("demo", help="run a small scripted scenario")
    demo.add_argument("--depth", type=int, default=6, help="book levels to display")
    demo.set_defaults(func=cmd_demo)

    stress = sub.add_parser("stress", help="insert N random orders and report throughput")
    stress.add_argument("--n", type=int, default=1_000_000, help="orders for the native run")
    stress.add_argument("--verify-n", type=int, default=20_000,
                        help="orders for the Python-API correctness pass")
    stress.add_argument("--seed", type=int, default=42)
    stress.set_defaults(func=cmd_stress)

    scenario = sub.add_parser("scenario", help="replay a scenario file")
    scenario.add_argument("--file", required=True, help="path to a scenario JSON file")
    scenario.add_argument("--depth", type=int, default=6)
    scenario.set_defaults(func=cmd_scenario)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
