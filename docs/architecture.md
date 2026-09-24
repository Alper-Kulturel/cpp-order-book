# Architecture

How the book is put together, and why each choice was made.

---

## 1. Matching model

### Price/time priority

The book is a **continuous, price/time-priority** limit order book — the
discipline used by essentially every cash equity and futures venue.

Two rules decide who trades:

1. **Price priority.** The most aggressive price wins. The best bid (highest)
   trades before any other bid; the best ask (lowest) trades before any other
   ask.
2. **Time priority.** At the same price, the order that arrived first trades
   first. This is FIFO within a price level.

The second rule is what makes a limit order book fair, and it is the reason a
level is a *queue* rather than a set. If two traders both rest 100 lots at
99.50, the one who got there first gets filled first. Without that guarantee,
posting a limit order would be a lottery and market makers could not price the
adverse-selection risk of resting liquidity.

### Continuous matching, and what `match()` does

Orders match **on entry**. Adding an order that crosses the spread fills it
immediately against the resting book, generating one trade per resting order it
consumes. Any unfilled remainder rests at its limit price.

That creates an API question: if matching happens inside `add_limit_order`, how
does the caller get the fills? The answer here is a small buffer:

- Every fill produced by the matching engine is appended to an internal
  `pending_trades_` buffer.
- `match()` returns that buffer's contents and clears it.
- `match()` also sweeps any residual cross before draining, as a safety net.

So `match()` answers "what has executed since I last asked?", which is what a
strategy actually wants to know, and the book is never left crossed between
calls. A caller that never calls `match()` just accumulates fills in the
buffer; the lifetime counters (`trades_executed()`, `volume_traded()`) stay
correct either way.

### Trade price

A trade executes at the **resting order's price**, not the incoming order's.
This is standard: the maker published a price and gets it; the taker crossed
the spread and gets price improvement.

```
resting bid 100.00  <-  incoming ask at 99.00
trade prints at 100.00
```

The seller was willing to sell at 99.00 and got 100.00. That improvement is the
compensation for taking liquidity.

### Partial fills

Sizes rarely match, so the engine fills `min(resting, incoming)` and:

- if the **resting** order is exhausted, it leaves the book;
- if the **incoming** order is exhausted, it stops and any remainder rests;
- if both are, both leave.

One aggressive order can walk arbitrarily many levels, producing one trade per
level touched. `test_deep_book_matching_through_many_levels` sweeps 200 levels
in a single order.

---

## 2. Data structures

### Overview

```cpp
std::pmr::map<Price, PriceLevel, std::greater<Price>> bids_;  // highest first
std::pmr::map<Price, PriceLevel, std::less<Price>>    asks_;  // lowest first
std::pmr::list<Order>                                 PriceLevel;
std::pmr::unordered_map<OrderId, OrderLocation>       index_;
```

### Why `std::map` for the price ladder

The book needs, repeatedly and on the hot path:

- the **best** price on each side (O(1) — it is `begin()`),
- the **next** price when a level is exhausted (O(1) — `++iterator`),
- **insertion** of a new price level (O(log L)),
- **removal** of an emptied level (O(log L)).

A sorted associative container gives all four. Ordered iteration is the
decisive property: when an aggressive order clears the 100.00 level it must
immediately find 100.01, and `std::map` iterators make that a pointer step
rather than a search. A hash map keyed by price would give O(1) lookup of a
*known* price but no way to ask "what is the next price up?".

`L` is the number of distinct price levels, not the number of orders. For a
single instrument that is typically tens to low thousands, so `log L` is a
handful of comparisons and the two comparators give each side its own ordering
without any special-casing in the matching loop: `bids_.begin()` is always the
highest bid and `asks_.begin()` is always the lowest ask.

The two comparators (`std::greater` for bids, `std::less` for asks) encode the
"best first" invariant in the type rather than in the code.

### Why `std::list` for a price level

Each level is a FIFO queue with a hostile access pattern:

- **append** at the back (new order arrives),
- **erase from the front** (order fills),
- **erase from the middle** (order cancels, or is re-queued).

`std::list` does all three in O(1) and, critically, **never invalidates
iterators to other elements**. That last property is what makes the O(1) cancel
index possible: the index can hold an iterator to an order and trust it stays
valid while other orders at the same level come and go.

A `std::deque` would be a reasonable alternative (O(1) push/erase at both ends)
but erasing from the middle is O(n), which a cancel is. A `std::vector` is
worse: erasing from the front is O(n) and any insert invalidates every iterator
in the level.

The cost of `std::list` is one node allocation per order and pointer-chasing
per traversal — addressed by the memory pool below.

### Why the id index stores iterators

The obvious index is `std::unordered_map<OrderId, Order>` — a copy of the
order. It gives an O(1) *lookup*, but a cancel then has to find and erase the
order inside its level's list, which is O(n) in the level size.

The index here stores **where the order lives**, not the order:

```cpp
struct OrderLocation {
    Side                 side;
    BidLevels::iterator  bid_level;
    AskLevels::iterator  ask_level;
    PriceLevel::iterator order;
};
```

A cancel is then genuinely O(1) end to end: hash lookup, jump straight to the
node, unlink it, and drop the level if it emptied.

Both level iterators are stored even though only one is ever live. Bids and
asks have different comparators and therefore different iterator types, so
keeping one would mean a type-erased or branchy alternative costing more than
the 8 wasted bytes per resting order.

`std::map` and `std::list` iterators are stable across all other inserts and
erases, and a level is only erased once its list is empty — at which point
every order that referenced it has already left the index. So the iterators
cannot dangle. `validate()` asserts exactly this by dereferencing each stored
iterator and checking it still points at the order it claims to.

### Memory: one pool for the whole book

Add and cancel are dominated by node allocation and deallocation. Every pricing
level, every order node and every index entry is therefore allocated from a
single `std::pmr::unsynchronized_pool_resource` owned by the book:

- **Unsynchronised** because the book is single-threaded by design — no atomics
  on the allocation path.
- **Pooled** because the sizes repeat (a handful of node sizes), so the
  resource hands back recycled blocks instead of going to `malloc`.
- Freed blocks return to the pool and are reused, so steady-state operation
  stops calling the general allocator entirely.
- The resource is a member declared **first**, so it is destroyed **last**, after
  the containers have returned their blocks.

Measured on the add+cancel churn path, this is roughly a 25% improvement over
the default allocator. `std::pmr` also keeps the "no raw `new`/`delete`" rule
intact: the only allocation calls in the codebase are inside the standard
library's resource.

---

## 3. Complexity

| Operation | Complexity | Why |
|---|---|---|
| `add_limit_order` | O(log L) + O(k) | level lookup, plus k fills if it crosses |
| `cancel_order` | **O(1)** | hash lookup, then unlink by stored iterator |
| `modify_order` (reduce) | **O(1)** | quantity update in place, queue position kept |
| `modify_order` (increase) | **O(1)** | `list::splice` relinks the node, no allocation |
| `best_bid` / `best_ask` | **O(1)** | `begin()` of an ordered map |
| `spread` / `mid_price` | **O(1)** | derived from the two `begin()`s |
| `bid_depth` / `ask_depth` | **O(1)** | maintained counters |
| `top_n_bids` / `top_n_asks` | O(n · k) | walks n levels, sums k orders each |
| `match` | O(fills) | drains the buffer |
| `validate` | O(n) | full re-derivation; debug only |

Codes: L = distinct price levels, k = orders consumed by one aggressive order,
n = levels requested, k = orders at those levels.

---

## 4. Order modification semantics

Exchanges distinguish two kinds of size change, and so does this book:

- **Reduction** — the order is smaller but wants the same place in the queue.
  It keeps its position. Nobody behind it is harmed, because it can only trade
  *less* than before.
- **Increase** — the order now wants to trade more than when it queued. If it
  kept its position it would jump ahead of everyone who queued behind it while
  it was small, so it is **re-queued at the back** of its level.

The re-queue uses `std::list::splice`, which relinks the existing node instead
of destroying and reallocating it — O(1), and the stored iterator stays valid,
so the index needs no update.

A priority-losing order is also **re-stamped**: its timestamp advances to the
moment it took its new queue position. A venue treats a priority-losing replace
as a fresh queue entry, and the timestamp is documented as describing the queue
position an order holds. Without the re-stamp the level's timestamps would no
longer be non-decreasing and `validate()` would (correctly) flag it — this is
precisely the bug the fuzz test in the suite caught during development.

`new_quantity == 0` is a cancel. A **price** change is not a modify: it is a
cancel plus a new order, because a different price is a different queue.

---

## 5. Price representation

Prices are `double`, as the interface requires. This is the one place where the
implementation is deliberately less rigorous than a production venue, and it is
worth being explicit about why:

- Binary floating point cannot represent most decimal prices exactly. `0.01` is
  not representable, so `100.0 + 0.01 * 3 != 100.03` in general.
- Using a float as a `std::map` key means two prices that "should" be equal can
  land in different levels, silently splitting liquidity.
- Comparisons of accumulated floating-point values are not transitive in edge
  cases, which can break the ordering invariant a sorted container depends on.

**What production books do instead:** represent price as a scaled integer —
integer ticks, or a fixed-point `int64_t` with an implied number of decimals.
Equality and ordering become exact and integer comparison is faster than
floating-point comparison. The conversion belongs at the API boundary: parse
`"99.50"` into `9950` ticks once, on the way in.

The `double` interface is preserved here because it makes the library pleasant
to drive from Python and matches the specified API. Swapping in integer ticks
is a contained change: the price type appears in the container declarations and
in the comparison helpers, not in the matching logic.

---

## 6. Threading

**The book is not thread-safe, and that is a design decision rather than an
omission.**

A limit order book is a single-writer structure. The matching engine is the one
component that must see a totally ordered stream of events; introducing a lock
would add contention and, worse, would let a reader observe a half-applied
match (an order removed but its trade not yet recorded). Even lock-free
approaches have to solve that consistency problem, not just the mutual
exclusion one.

The real architecture is:

- one thread owns the book and applies events from the feed in sequence;
- other threads receive snapshots, publish market data, or run strategies.

If several threads genuinely must mutate one book, the correct fix is external
serialisation — a single-producer queue feeding the owning thread — not
fine-grained locking inside the data structure.

---

## 7. What a faster book would do next

The implementation is honest about being a well-engineered general-purpose book
rather than a hand-tuned HFT engine. Ranked by expected payoff:

1. **Integer tick prices.** See §5. Removes floating-point hashing and
   comparison from the hot path and makes levels exactly comparable.
2. **Array-indexed price ladder.** Replace `std::map` with a flat array indexed
   by `(price − base) / tick`, with a best-price cursor that walks up and down.
   Level access becomes a single indexed load instead of a tree descent of
   pointer-chasing comparisons, which is where most of the remaining cache
   misses are.
3. **Intrusive queues.** Put the `next`/`prev` links inside `Order` and keep
   orders in a preallocated slab, removing the separate list node allocation
   and one pointer indirection per traversal.
4. **Custom open-addressing index.** Replace `std::unordered_map` with a flat
   open-addressed table sized to a power of two. The standard node-based hash
   map allocates per entry and chases pointers on every lookup.
5. **Feed timestamps from the wire.** Already done here — the caller supplies
   timestamps and the clock is off the measured path, worth ~16 ns per order.
6. **Batch/pipeline the matching loop.** Amortise branch mispredictions across
   a batch of incoming orders, and keep the top of book in a small
   cache-resident structure that is updated incrementally.

Items 1 and 2 are the big ones; together they are the difference between a book
that runs in ~70 ns per operation and one that runs in single-digit
nanoseconds.

---

## 8. Self-checking

`validate()` re-derives the entire book state from first principles and
compares it against the incrementally maintained state. It checks:

- every level is non-empty, and its key is a valid price;
- bid levels are strictly descending, ask levels strictly ascending;
- every order sits at its level's price and on the right side;
- **within a level, timestamps are non-decreasing** (the FIFO invariant);
- no zero-quantity order rests;
- the id index contains exactly the resting orders, with matching side, and
  each stored iterator still points at the order it claims to;
- the depth and volume counters equal a recount;
- the book is not crossed.

It is O(n), for tests and debugging — never the hot path. The fuzz test calls
it after *every* operation of a 4,000-step randomised add/cancel/modify
workload, which is how the re-stamp bug in §4 was found.
