# C++ Limit Order Book

A price/time-priority limit order book in modern C++17, with pybind11 bindings,
a Python CLI, latency benchmarks and a pytest suite.

The matching engine is C++ and runs at **~67 ns per insert** and **~194 ns per
random cancel** on an Apple M1 (≈15M and ≈5M ops/sec respectively). Python
drives it; Python is not in the hot path.

---

## Table of contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Operations](#operations)
- [Benchmark results](#benchmark-results)
- [Build](#build)
- [Usage](#usage)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Design notes and limitations](#design-notes-and-limitations)

---

## Overview

A limit order book is the data structure at the centre of every electronic
exchange: it holds all outstanding buy and sell interest for one instrument and
decides who trades with whom, at what price, and in what order.

This implementation supports:

- **Continuous matching.** An order that crosses the spread fills immediately
  against the resting book, walking as many levels as its size requires.
- **Price/time priority.** Best price first; at the same price, FIFO.
- **Partial fills.** Sizes rarely match, so remainders rest and resting orders
  are decremented in place.
- **O(1) cancel by id**, backed by a hash index that stores iterators rather
  than copies.
- **Modify with exchange semantics** — a size reduction keeps queue position, a
  size increase is re-queued at the back.
- **Self-validation.** `validate()` re-derives all state and compares, so the
  invariants are testable rather than merely intended.

The book is deliberately **single-threaded** (see
[Design notes](#design-notes-and-limitations)).

---

## Architecture

```
                    add_limit_order(side, price, qty)
                                |
                                v
              +-----------------------------------+
              |  level lookup  O(log L)           |
              |  bids_ : map<price, list, greater>|
              |  asks_ : map<price, list, less>   |
              +-----------------------------------+
                                |
                                v
              +-----------------------------------+
              |  matching engine                  |
              |  while crossed: fill min(bid,ask) |
              |  trade prints at PASSIVE price    |
              +-----------------------------------+
                       |                    |
                       v                    v
              +----------------+   +---------------------+
              | pending trades |   | index_              |
              | (drained by    |   | unordered_map<id,   |
              |  match())      |   |   OrderLocation>    |
              +----------------+   | O(1) cancel         |
                                   +---------------------+
```

The three core structures and why each was chosen:

| Structure | Role | Why |
|---|---|---|
| `std::pmr::map<Price, PriceLevel, greater/less>` | price ladder | ordered iteration gives O(1) best price (`begin()`) and O(1) next level (`++`), which a hash map cannot |
| `std::pmr::list<Order>` | FIFO queue per level | O(1) append, erase-from-front and erase-from-middle, and **iterators to other elements stay valid** |
| `std::pmr::unordered_map<OrderId, OrderLocation>` | id index | O(1) cancel: the entry stores an iterator straight to the order's node, not a copy |

All three allocate from a single `std::pmr::unsynchronised_pool_resource` owned
by the book, which keeps the general-purpose allocator off the add/cancel path
(~25% faster churn than the default allocator) and means the codebase contains
no raw `new`/`delete`.

Full reasoning, complexity table and trade-offs: **[docs/architecture.md](docs/architecture.md)**.

---

## Operations

| Method | Returns | Notes |
|---|---|---|
| `add_limit_order(side, price, quantity, timestamp=0)` | `OrderId` | matches on entry; `0` if rejected |
| `cancel_order(order_id)` | `bool` | `True` if it was resting and is now gone |
| `modify_order(order_id, new_quantity, timestamp=0)` | `bool` | reduce keeps priority, increase re-queues |
| `match()` | `list[Trade]` | trades since the last call; clears the buffer |
| `best_bid()` / `best_ask()` | `float` | `NaN` when that side is empty |
| `spread()` / `mid_price()` | `float` | `NaN` if either side is empty |
| `bid_depth()` / `ask_depth()` | `int` | resting **order count** |
| `bid_volume()` / `ask_volume()` | `int` | resting **quantity** |
| `bid_levels()` / `ask_levels()` | `int` | distinct price levels |
| `top_n_bids(n)` / `top_n_asks(n)` | `list[(price, qty)]` | best first |
| `level_orders(side, price)` | `list[Order]` | queue order, front first |
| `find_order(order_id)` | `Order` or `None` | copy of the resting order |
| `contains(order_id)` | `bool` | |
| `validate()` | `str` | `""` means consistent, else the first problem |
| `clear()` | — | reset to a fresh book, keeping the pool |

Rejected orders (`quantity == 0`, or a price that is not finite and positive)
return `INVALID_ORDER_ID` (`0`) and never touch the book. **No exceptions are
thrown on the hot path.**

Inside a price level, orders are held in strict FIFO order and the timestamp
records the queue position an order currently holds — a priority-losing modify
re-stamps it.

---

## Benchmark results

Apple M1, Apple clang 15.0.0, `-O3 -march=native`, 1,000,000 operations per
scenario, median of 5 runs (seed 42). Reproduce with
`python3 benchmarks/benchmark.py`; full report in
**[benchmarks/results.md](benchmarks/results.md)**.

### C++ core (GIL released)

| Scenario | Avg latency | Throughput | Trades |
|---|---:|---:|---:|
| `add` — passive inserts | **66.6 ns/op** | **15,019,939 ops/sec** | 0 |
| `cancel` — random-order cancels | **193.5 ns/op** | **5,166,634 ops/sec** | 0 |
| `match` — aggressive orders, one trade each | **88.9 ns/op** | **11,244,620 ops/sec** | 1,000,000 |
| `mixed` — 70/20/10 add/cancel/aggressive | **94.2 ns/op** | **10,611,069 ops/sec** | 50,464 |
| `clock` — one `now_ns()` call | 16.0 ns/op | 62,387,087 ops/sec | — |

### Python round-trip

| Path | Avg latency | Throughput |
|---|---:|---:|
| `add` from C++ (GIL released) | 66.6 ns/op | 15,019,939 ops/sec |
| `add` from Python (per call) | 477.8 ns/op | 2,092,969 ops/sec |

Crossing the binding costs **~411 ns per call** — that is the interpreter and
the pybind11 trampoline, not the order book. Throughput-critical work therefore
runs in C++ and Python drives it in bulk, which is exactly the split
`bench_driver` demonstrates.

Two measurement details worth stating, because both are easy ways to publish a
flattering number:

- **The clock is not on the measured path.** `now_ns()` costs 16 ns, a large
  fraction of a 67 ns insert. A real feed handler stamps orders from the wire,
  so the harness passes explicit timestamps and the book never reads the clock.
- **Setup is never timed.** Populating a million resting orders happens outside
  the measured region.

`cancel` is the slowest scenario for a reason worth understanding: 1,000,000
resting orders are cancelled in random order, so nearly every unlink touches
cold memory. The operation is O(1) — the cost is cache misses across a
~200 MB working set, not algorithmic. A book with a hot working set cancels
considerably faster; at n=100,000 the same scenario runs at ~57 ns/op.

---

## Build

### Requirements

- CMake ≥ 3.16
- A C++17 compiler (tested with Apple clang 15 / clang 1500.3.9.4)
- Python ≥ 3.8 with `pybind11` (`pip install pybind11`)

### Steps

```bash
# 1. configure (Release enables -O3 -march=native)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. build the library, the native benchmark and both Python modules
cmake --build build --config Release -j 4

# 3. the .so files are copied into python/ automatically by the build.
#    To do it by hand instead:
#      cp build/order_book_cpp*.so python/

# 4. verify
cd python && python3 -c "import order_book_cpp; ob = order_book_cpp.OrderBook(); print('OK')"
```

CMake finds pybind11 by asking the `python3` on your `PATH` for its cmake
directory, so an activated virtualenv Just Works. To build against a specific
interpreter, pass it explicitly:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DPython3_EXECUTABLE=/path/to/python3 \
      -Dpybind11_DIR=$(/path/to/python3 -m pybind11 --cmakedir)
```

### Targets

| Target | What it is |
|---|---|
| `lob` | static library, the order book itself (no Python) |
| `lob_bench` | native benchmark binary |
| `order_book_cpp` | pybind11 module — the book |
| `bench_driver` | pybind11 module — the benchmark harness |

Because the native benchmark binary shares `bench_core.hpp` with the Python
harness, the two can never report different numbers for the same code:

```bash
./build/lob_bench 1000000 42
```

---

## Usage

### Python

```python
import order_book_cpp as lob

book = lob.OrderBook()

# Rest some liquidity.
bid = book.add_limit_order(side=lob.Side.Bid, price=99.50, quantity=100)
ask = book.add_limit_order(side=lob.Side.Ask, price=100.50, quantity=100)

book.best_bid(), book.best_ask()   # (99.5, 100.5)
book.spread(), book.mid_price()    # (1.0, 100.0)
book.top_n_bids(3)                 # [(99.5, 100)]

# An aggressive sell crosses the spread and trades at the *resting* price.
book.add_limit_order(side=lob.Side.Ask, price=99.00, quantity=40)
for trade in book.match():
    print(trade)
# <Trade buy=1 sell=3 price=99.5 qty=40>   <- seller gets 99.50, not 99.00

book.find_order(order_id=bid).quantity     # 60 left
book.validate()                            # '' means consistent
```

Identical API in C++:

```cpp
#include "order_book.hpp"

lob::OrderBook book;
const lob::OrderId id = book.add_limit_order(lob::Side::Bid, 99.50, 100);
std::vector<lob::Trade> trades = book.match();
book.modify_order(id, 50);   // reduction: keeps queue position
```

### CLI

```bash
cd python
python3 cli.py demo                                  # scripted tour
python3 cli.py stress --n 1000000                    # throughput
python3 cli.py scenario --file ../scenarios/example.json
```

`demo` walks through price/time priority, a partial fill that sweeps two levels,
modify semantics and cancels, printing the ladder after each step:

```
   ASK     100.30  x      150
   ASK     100.20  x      600
   ASK     100.10  x      250
  ----------------------------------  spread 0.20   mid 100.00
   BID      99.90  x      300
   BID      99.80  x      700
   BID      99.70  x      400
```

### Scenario files

A scenario is a JSON list of operations, replayed in order. Order ids are
assigned sequentially from 1, so a scenario can cancel or modify what it added:

```json
[
  {"op": "add",    "side": "bid", "price": 99.50, "qty": 100},
  {"op": "add",    "side": "ask", "price": 100.50, "qty": 150},
  {"op": "modify", "id": 1, "qty": 50},
  {"op": "cancel", "id": 2},
  {"op": "match"},
  {"op": "depth",  "n": 5}
]
```

Supported ops: `add`, `cancel`, `modify`, `match`, `depth`. See
[scenarios/example.json](scenarios/example.json).

---

## Testing

```bash
python -m pytest tests/ -v
```

53 tests covering best-price accessors, crossing and non-crossing orders,
trade pricing, partial fills, walking multiple levels, cancel/not-found,
modify semantics (including the queue-position rules), spread and mid-price,
top-N ordering and aggregation, invalid-input rejection, `clear()`, timestamp
handling, and validation.

Two of them are stress tests:

- **10,000 random orders** in non-crossing bands, asserting the book never
  crosses (`best_bid < best_ask`) and that every order cancels cleanly.
- **A 4,000-step randomised add/cancel/modify fuzz** against a crossing-prone
  price band, calling `validate()` after *every single operation*. This is the
  test that found the re-queue timestamp bug described in
  [docs/architecture.md](docs/architecture.md) §4.

---

## Project layout

```
cpp-order-book/
├── CMakeLists.txt              library + both pybind11 modules
├── README.md
├── .gitignore
├── cpp/
│   ├── include/
│   │   ├── order_book.hpp      public API
│   │   └── bench_core.hpp      benchmark harness (shared by both frontends)
│   ├── src/
│   │   ├── order_book.cpp      matching engine + pool-backed containers
│   │   └── bench_main.cpp      native benchmark binary
│   ├── bindings.cpp            pybind11 module: order_book_cpp
│   └── bench_driver.cpp        pybind11 module: bench_driver
├── python/
│   ├── cli.py                  demo / stress / scenario
│   └── order_book_cpp*.so      built artefacts (gitignored)
├── tests/
│   ├── conftest.py
│   └── test_order_book.py      53 tests
├── benchmarks/
│   ├── benchmark.py            runs the suite, writes the report
│   └── results.md              generated
├── scenarios/
│   └── example.json
└── docs/
    └── architecture.md         data structures, complexity, trade-offs
```

---

## Design notes and limitations

**Not thread-safe — by design.** A book is a single-writer structure: one
thread owns it and applies feed events in sequence, while other threads consume
snapshots. Adding a lock would not just cost contention, it would allow a
reader to observe a half-applied match. Concurrency belongs outside, via a
single-producer queue into the owning thread.

**Prices are `double`.** The specified interface uses floating point, but a
production venue uses scaled integers: `0.01` is not exactly representable, and
using floats as `std::map` keys means prices that should be equal can land in
different levels and silently split liquidity. The fix is contained — parse to
integer ticks at the API boundary. Discussed in
[docs/architecture.md](docs/architecture.md) §5.

**Move-assignment is deleted.** The containers hold a raw pointer to the book's
`pmr` pool, and `polymorphic_allocator` propagates on move-assignment — a
member-wise move-assign would destroy the pool before the containers returned
their blocks to it. Move *construction* is fine and is supported.

**Scanning is O(n).** `top_n_*` sums a level by walking its orders, which is
fine for the small levels a real book has. A price level aggregated to a single
"total quantity" node would make it O(1) at the cost of another invariant to
maintain on every fill.

**No self-trade prevention, no order types beyond limit, no icebergs, no
auctions.** Limit orders only, as specified. Market orders are expressible as a
deeply-priced limit; an IOC would be a flag that skips the resting step.

---

## License

Provided as-is for evaluation and educational use.
