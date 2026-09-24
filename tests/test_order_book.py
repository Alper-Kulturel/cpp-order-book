"""pytest suite for the C++ limit order book.

Run from the repo root:

    python -m pytest tests/ -v

Every test drives the compiled extension through its Python API, so this
exercises the C++ core, the pybind11 bindings and the value conversions
together. The final tests fuzz the book and assert its O(n) self-check
(``validate()``) after every single operation.
"""

from __future__ import annotations

import math
import random
from typing import List, Sequence

import pytest

import order_book_cpp as lob

Bid = lob.Side.Bid
Ask = lob.Side.Ask


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def book_with(bids: Sequence[tuple] = (), asks: Sequence[tuple] = ()) -> "lob.OrderBook":
    """Build a book from (price, quantity) pairs. Returns the book."""
    book = lob.OrderBook()
    for price, qty in bids:
        book.add_limit_order(side=Bid, price=price, quantity=qty)
    for price, qty in asks:
        book.add_limit_order(side=Ask, price=price, quantity=qty)
    book.match()  # drop the fills generated while building
    return book


def queue_at(book: "lob.OrderBook", side, price: float) -> List[int]:
    return [order.id for order in book.level_orders(side=side, price=price)]


# ---------------------------------------------------------------------------
# basic accessors
# ---------------------------------------------------------------------------


def test_empty_book_reports_nan_not_zero():
    book = lob.OrderBook()
    for value in (book.best_bid(), book.best_ask(), book.spread(), book.mid_price()):
        assert math.isnan(value)
    assert book.empty()
    assert not book.has_bids()
    assert not book.has_asks()
    assert book.total_orders() == 0
    assert book.bid_depth() == 0 and book.ask_depth() == 0
    assert book.validate() == ""


def test_add_bid_and_ask_sets_best_prices():
    book = lob.OrderBook()
    book.add_limit_order(side=Bid, price=99.5, quantity=100)
    book.add_limit_order(side=Ask, price=100.5, quantity=100)

    assert book.best_bid() == 99.5
    assert book.best_ask() == 100.5
    assert book.has_bids() and book.has_asks()
    assert not book.empty()


def test_best_price_is_the_aggressive_end_of_each_side():
    book = book_with(bids=[(99.0, 10), (99.9, 10), (99.5, 10)],
                     asks=[(100.9, 10), (100.1, 10), (100.5, 10)])

    assert book.best_bid() == 99.9  # highest bid
    assert book.best_ask() == 100.1  # lowest ask


def test_new_order_ids_are_unique_and_increasing():
    book = lob.OrderBook()
    ids = [book.add_limit_order(side=Bid, price=99.0, quantity=1) for _ in range(5)]
    assert len(set(ids)) == 5
    assert ids == sorted(ids)
    assert book.last_order_id() == ids[-1]


# ---------------------------------------------------------------------------
# matching
# ---------------------------------------------------------------------------


def test_crossing_order_executes_a_trade():
    book = lob.OrderBook()
    buy = book.add_limit_order(side=Bid, price=100.0, quantity=50)
    book.match()
    sell = book.add_limit_order(side=Ask, price=99.0, quantity=50)

    trades = book.match()

    assert len(trades) == 1
    trade = trades[0]
    assert isinstance(trade, lob.Trade)
    assert trade.buy_order_id == buy
    assert trade.sell_order_id == sell
    assert trade.quantity == 50
    assert book.trades_executed() == 1
    assert book.volume_traded() == 50
    assert book.empty()


def test_trade_price_is_the_resting_orders_price():
    """The taker gets price improvement; the maker never trades through their limit."""
    book = lob.OrderBook()
    book.add_limit_order(side=Bid, price=100.0, quantity=50)  # resting buy
    book.match()

    book.add_limit_order(side=Ask, price=99.0, quantity=50)  # willing to sell at 99.0
    trade = book.match()[0]

    assert trade.price == 100.0  # executes at the resting bid, not the ask
    assert book.last_trade_price() == 100.0


def test_non_crossing_order_does_not_trade():
    book = lob.OrderBook()
    book.add_limit_order(side=Bid, price=100.0, quantity=50)
    book.add_limit_order(side=Ask, price=100.5, quantity=50)

    assert book.match() == []
    assert book.trades_executed() == 0
    assert book.bid_depth() == 1 and book.ask_depth() == 1


def test_order_crossing_at_the_same_price_trades():
    """A bid and an ask at the same price cross -- the spread is zero."""
    book = lob.OrderBook()
    book.add_limit_order(side=Bid, price=100.0, quantity=10)
    book.match()
    book.add_limit_order(side=Ask, price=100.0, quantity=10)

    assert len(book.match()) == 1
    assert book.empty()


def test_aggressive_order_walks_multiple_levels():
    book = book_with(asks=[(100.0, 10), (100.1, 10), (100.2, 10)])

    book.add_limit_order(side=Bid, price=100.2, quantity=25)
    trades = book.match()

    # It fills 10 + 10 + 5, taking 5 out of the third level at its own limit.
    assert [t.price for t in trades] == [100.0, 100.1, 100.2]
    assert [t.quantity for t in trades] == [10, 10, 5]
    assert sum(t.quantity for t in trades) == 25

    # 25 units against 30 available, so the taker is filled outright and
    # nothing rests on the bid side.
    assert math.isnan(book.best_bid())
    assert book.best_ask() == 100.2
    assert book.ask_volume() == 5
    assert book.validate() == ""


def test_match_drains_its_buffer_and_is_idempotent():
    book = lob.OrderBook()
    book.add_limit_order(side=Bid, price=100.0, quantity=10)
    book.match()
    book.add_limit_order(side=Ask, price=100.0, quantity=10)

    assert len(book.match()) == 1
    assert book.match() == []  # nothing new since the last drain
    assert book.trades_executed() == 1  # but the lifetime counter is unchanged


def test_match_returns_a_plain_python_list_of_trades():
    book = lob.OrderBook()
    book.add_limit_order(side=Bid, price=100.0, quantity=10)
    book.match()
    book.add_limit_order(side=Ask, price=100.0, quantity=10)

    trades = book.match()
    assert isinstance(trades, list)
    assert all(isinstance(t, lob.Trade) for t in trades)


# ---------------------------------------------------------------------------
# partial fills
# ---------------------------------------------------------------------------


def test_partial_fill_reduces_the_resting_order():
    book = lob.OrderBook()
    resting = book.add_limit_order(side=Bid, price=100.0, quantity=100)
    book.match()

    book.add_limit_order(side=Ask, price=100.0, quantity=40)
    trades = book.match()

    assert trades[0].quantity == 40
    remaining = book.find_order(order_id=resting)
    assert remaining is not None
    assert remaining.quantity == 60  # 100 - 40
    assert book.bid_volume() == 60
    assert book.bid_depth() == 1
    assert book.validate() == ""


def test_partially_filled_aggressor_rests_its_remainder():
    book = book_with(asks=[(100.0, 30)])

    aggressor = book.add_limit_order(side=Bid, price=101.0, quantity=100)
    trades = book.match()

    assert len(trades) == 1 and trades[0].quantity == 30
    assert book.has_asks() is False
    resting = book.find_order(order_id=aggressor)
    assert resting is not None and resting.quantity == 70
    assert book.best_bid() == 101.0


def test_full_fill_removes_the_order_from_the_book():
    book = lob.OrderBook()
    resting = book.add_limit_order(side=Bid, price=100.0, quantity=100)
    book.match()
    book.add_limit_order(side=Ask, price=100.0, quantity=100)
    book.match()

    assert not book.contains(resting)
    assert book.find_order(order_id=resting) is None
    assert book.empty()
    assert book.bid_volume() == 0


# ---------------------------------------------------------------------------
# cancel
# ---------------------------------------------------------------------------


def test_cancel_existing_order_returns_true():
    book = lob.OrderBook()
    order_id = book.add_limit_order(side=Bid, price=99.0, quantity=10)

    assert book.cancel_order(order_id=order_id) is True
    assert not book.contains(order_id)
    assert book.empty()
    assert book.bid_depth() == 0


def test_cancel_missing_order_returns_false():
    book = lob.OrderBook()
    assert book.cancel_order(order_id=12345) is False

    listed = book.add_limit_order(side=Bid, price=99.0, quantity=10)
    book.cancel_order(order_id=listed)
    assert book.cancel_order(order_id=listed) is False  # second cancel is a no-op
    assert book.cancel_order(order_id=0) is False  # the invalid-id sentinel


def test_cancel_removes_the_price_level_when_it_empties():
    book = lob.OrderBook()
    first = book.add_limit_order(side=Bid, price=99.0, quantity=10)
    second = book.add_limit_order(side=Bid, price=99.0, quantity=10)

    book.cancel_order(order_id=first)
    assert book.bid_levels() == 1  # level survives, one order left

    book.cancel_order(order_id=second)
    assert book.bid_levels() == 0  # level is gone
    assert book.bid_volume() == 0


def test_cancel_does_not_disturb_the_rest_of_the_level():
    book = lob.OrderBook()
    first = book.add_limit_order(side=Bid, price=99.0, quantity=10)
    second = book.add_limit_order(side=Bid, price=99.0, quantity=20)
    third = book.add_limit_order(side=Bid, price=99.0, quantity=30)

    book.cancel_order(order_id=second)

    assert queue_at(book, Bid, 99.0) == [first, third]
    assert book.bid_volume() == 40
    assert book.validate() == ""


# ---------------------------------------------------------------------------
# modify
# ---------------------------------------------------------------------------


def test_modify_reduces_quantity_and_reports_success():
    book = lob.OrderBook()
    order_id = book.add_limit_order(side=Bid, price=99.0, quantity=100)

    assert book.modify_order(order_id=order_id, new_quantity=40) is True

    order = book.find_order(order_id=order_id)
    assert order is not None and order.quantity == 40
    assert book.bid_volume() == 40
    assert book.validate() == ""


def test_modify_missing_order_returns_false():
    book = lob.OrderBook()
    assert book.modify_order(order_id=777, new_quantity=10) is False


def test_modify_to_zero_cancels_the_order():
    book = lob.OrderBook()
    order_id = book.add_limit_order(side=Bid, price=99.0, quantity=100)

    assert book.modify_order(order_id=order_id, new_quantity=0) is True
    assert not book.contains(order_id)
    assert book.empty()


def test_modify_reduction_keeps_time_priority():
    book = lob.OrderBook()
    first = book.add_limit_order(side=Bid, price=100.0, quantity=10)
    second = book.add_limit_order(side=Bid, price=100.0, quantity=10)

    book.modify_order(order_id=first, new_quantity=5)
    assert queue_at(book, Bid, 100.0) == [first, second]

    book.match()
    book.add_limit_order(side=Ask, price=100.0, quantity=5)
    assert book.match()[0].buy_order_id == first  # still at the front


def test_modify_increase_sends_the_order_to_the_back_of_the_queue():
    book = lob.OrderBook()
    first = book.add_limit_order(side=Bid, price=100.0, quantity=10)
    second = book.add_limit_order(side=Bid, price=100.0, quantity=10)

    book.modify_order(order_id=first, new_quantity=50)

    assert queue_at(book, Bid, 100.0) == [second, first]
    book.match()
    book.add_limit_order(side=Ask, price=100.0, quantity=10)
    assert book.match()[0].buy_order_id == second  # queued ahead now


def test_modify_increase_restamps_the_order_to_its_new_queue_position():
    """A priority-losing replace is a new queue entry, so its clock advances."""
    book = lob.OrderBook()
    first = book.add_limit_order(side=Bid, price=100.0, quantity=10, timestamp=100)
    second = book.add_limit_order(side=Bid, price=100.0, quantity=10, timestamp=200)

    book.modify_order(order_id=first, new_quantity=50, timestamp=300)

    assert queue_at(book, Bid, 100.0) == [second, first]
    assert book.find_order(order_id=first).timestamp == 300
    assert book.find_order(order_id=second).timestamp == 200
    assert book.validate() == ""


def test_modify_reduction_keeps_the_original_timestamp():
    book = lob.OrderBook()
    order_id = book.add_limit_order(side=Bid, price=100.0, quantity=10, timestamp=100)

    book.modify_order(order_id=order_id, new_quantity=5, timestamp=300)

    assert book.find_order(order_id=order_id).timestamp == 100  # untouched
    assert book.validate() == ""


def test_modify_does_not_change_the_orders_price_or_side():
    book = lob.OrderBook()
    order_id = book.add_limit_order(side=Ask, price=101.0, quantity=10)

    book.modify_order(order_id=order_id, new_quantity=999)

    order = book.find_order(order_id=order_id)
    assert order is not None
    assert order.price == 101.0
    assert order.side == Ask
    assert book.best_ask() == 101.0
    assert book.ask_volume() == 999


# ---------------------------------------------------------------------------
# spread / mid price
# ---------------------------------------------------------------------------


def test_spread_and_mid_price_are_computed_from_the_touch():
    book = book_with(bids=[(99.0, 10), (99.5, 10)], asks=[(100.5, 10), (101.0, 10)])

    assert book.spread() == pytest.approx(1.0)
    assert book.mid_price() == pytest.approx(100.0)
    assert book.spread() == pytest.approx(book.best_ask() - book.best_bid())


def test_spread_and_mid_are_nan_when_either_side_is_empty():
    one_sided = book_with(bids=[(99.0, 10)])
    assert one_sided.best_bid() == 99.0
    assert math.isnan(one_sided.best_ask())
    assert math.isnan(one_sided.spread())
    assert math.isnan(one_sided.mid_price())


def test_touch_moves_as_levels_are_consumed():
    book = book_with(bids=[(99.0, 10), (98.0, 10)], asks=[(100.0, 10)])

    book.add_limit_order(side=Ask, price=99.0, quantity=10)  # lift the best bid
    book.match()

    assert book.best_bid() == 98.0
    assert book.spread() == pytest.approx(2.0)


# ---------------------------------------------------------------------------
# depth / top-N
# ---------------------------------------------------------------------------


def test_top_n_bids_are_sorted_best_first():
    book = book_with(bids=[(98.0, 10), (99.5, 20), (99.0, 30)])

    assert [price for price, _ in book.top_n_bids(10)] == [99.5, 99.0, 98.0]
    assert [qty for _, qty in book.top_n_bids(10)] == [20, 30, 10]


def test_top_n_asks_are_sorted_best_first():
    book = book_with(asks=[(101.0, 10), (100.5, 20), (102.0, 30)])

    assert [price for price, _ in book.top_n_asks(10)] == [100.5, 101.0, 102.0]
    assert [qty for _, qty in book.top_n_asks(10)] == [20, 10, 30]


def test_top_n_aggregates_all_orders_at_a_price():
    book = book_with(bids=[(99.0, 10), (99.0, 15), (98.0, 5)])

    assert book.top_n_bids(10) == [(99.0, 25), (98.0, 5)]


def test_top_n_respects_n():
    book = book_with(bids=[(99.0, 1), (98.0, 1), (97.0, 1)])

    assert len(book.top_n_bids(2)) == 2
    assert book.top_n_bids(2) == [(99.0, 1), (98.0, 1)]


def test_top_n_with_n_larger_than_the_book():
    book = book_with(bids=[(99.0, 1)])

    assert book.top_n_bids(100) == [(99.0, 1)]
    assert book.top_n_asks(100) == []


def test_top_n_of_an_empty_book_is_empty():
    book = lob.OrderBook()
    assert book.top_n_bids(5) == []
    assert book.top_n_asks(5) == []


def test_top_n_ignores_partially_filled_quantity():
    book = book_with(bids=[(100.0, 100)])
    book.add_limit_order(side=Ask, price=100.0, quantity=30)
    book.match()

    assert book.top_n_bids(1) == [(100.0, 70)]


# ---------------------------------------------------------------------------
# queue order / introspection
# ---------------------------------------------------------------------------


def test_price_time_priority_is_fifo_within_a_level():
    book = lob.OrderBook()
    first = book.add_limit_order(side=Bid, price=100.0, quantity=10)
    second = book.add_limit_order(side=Bid, price=100.0, quantity=10)
    third = book.add_limit_order(side=Bid, price=100.0, quantity=10)

    assert queue_at(book, Bid, 100.0) == [first, second, third]

    book.match()
    book.add_limit_order(side=Ask, price=100.0, quantity=20)
    trades = book.match()

    # The two oldest orders fill in arrival order; the newest is untouched.
    assert [t.buy_order_id for t in trades] == [first, second]
    assert [t.quantity for t in trades] == [10, 10]
    assert book.contains(third)
    assert queue_at(book, Bid, 100.0) == [third]


def test_time_priority_is_per_order_not_per_level():
    """A later order at a better price still wins over an earlier worse one."""
    book = lob.OrderBook()
    worse = book.add_limit_order(side=Bid, price=99.0, quantity=10)
    better = book.add_limit_order(side=Bid, price=99.9, quantity=10)

    book.match()
    book.add_limit_order(side=Ask, price=99.0, quantity=10)
    trades = book.match()

    assert trades[0].buy_order_id == better  # price priority beats time priority
    assert book.contains(worse)


def test_level_orders_is_empty_for_an_unknown_price():
    book = book_with(bids=[(99.0, 10)])
    assert book.level_orders(side=Bid, price=123.0) == []
    assert book.level_orders(side=Ask, price=99.0) == []  # right price, wrong side


# ---------------------------------------------------------------------------
# depth / volume bookkeeping
# ---------------------------------------------------------------------------


def test_depth_and_volume_counters_match_a_brute_force_recount():
    book = book_with(bids=[(99.0, 10), (99.0, 20), (98.0, 5)], asks=[(101.0, 7)])

    assert book.bid_depth() == 3
    assert book.ask_depth() == 1
    assert book.total_orders() == 4
    assert book.bid_volume() == 35
    assert book.ask_volume() == 7
    assert book.bid_levels() == 2
    assert book.ask_levels() == 1


# ---------------------------------------------------------------------------
# defensive behaviour
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "price,quantity",
    [
        (100.0, 0),  # zero quantity
        (0.0, 10),  # zero price
        (-1.0, 10),  # negative price
        (float("nan"), 10),  # NaN price
        (float("inf"), 10),  # infinite price
    ],
)
def test_invalid_orders_are_rejected_and_leave_the_book_untouched(price, quantity):
    book = lob.OrderBook()

    order_id = book.add_limit_order(side=Bid, price=price, quantity=quantity)

    assert order_id == lob.INVALID_ORDER_ID
    assert book.empty()
    assert book.validate() == ""


def test_rejected_order_does_not_consume_an_id():
    book = lob.OrderBook()
    assert book.add_limit_order(side=Bid, price=0.0, quantity=10) == lob.INVALID_ORDER_ID
    assert book.add_limit_order(side=Bid, price=99.0, quantity=10) == 1


def test_clear_resets_the_book():
    book = book_with(bids=[(99.0, 10)], asks=[(101.0, 10)])
    book.add_limit_order(side=Ask, price=99.0, quantity=10)
    book.match()
    assert book.trades_executed() > 0

    book.clear()

    assert book.empty()
    assert book.trades_executed() == 0
    assert book.volume_traded() == 0
    assert book.bid_volume() == 0 and book.ask_volume() == 0
    assert book.validate() == ""
    assert book.add_limit_order(side=Bid, price=99.0, quantity=1) == 1  # ids restart


def test_explicit_timestamps_are_preserved():
    book = lob.OrderBook()
    first = book.add_limit_order(side=Bid, price=100.0, quantity=10, timestamp=111)
    second = book.add_limit_order(side=Bid, price=100.0, quantity=10, timestamp=222)

    assert book.find_order(order_id=first).timestamp == 111
    assert book.find_order(order_id=second).timestamp == 222
    assert queue_at(book, Bid, 100.0) == [first, second]


def test_auto_timestamp_is_monotonic():
    book = lob.OrderBook()
    ids = [book.add_limit_order(side=Bid, price=99.0 - i * 0.01, quantity=1) for i in range(50)]

    stamps = [book.find_order(order_id=i).timestamp for i in ids]
    assert stamps == sorted(stamps)


# ---------------------------------------------------------------------------
# stress
# ---------------------------------------------------------------------------


def test_stress_10000_orders_keeps_the_book_uncrossed():
    """Insert 10,000 orders into non-crossing bands and verify the invariants."""
    book = lob.OrderBook(20_000)
    rng = random.Random(20240607)

    live: List[int] = []
    for _ in range(10_000):
        side = Bid if rng.random() < 0.5 else Ask
        if side == Bid:
            price = round(rng.uniform(90.0, 99.99), 2)
        else:
            price = round(rng.uniform(100.01, 110.0), 2)
        order_id = book.add_limit_order(side=side, price=price, quantity=rng.randint(1, 100))
        assert order_id != lob.INVALID_ORDER_ID
        live.append(order_id)

    assert book.total_orders() == 10_000
    assert book.match() == []  # nothing crossed by construction

    # The defining invariant of a well-formed book.
    assert book.best_bid() < book.best_ask()
    assert book.spread() > 0
    assert book.validate() == ""

    # Every id is still reachable and cancels cleanly.
    for order_id in live:
        assert book.cancel_order(order_id=order_id) is True
    assert book.empty()
    assert book.validate() == ""


def test_stress_random_workload_stays_consistent_after_every_operation():
    """Fuzz add/cancel/modify against a crossing-prone price band."""
    book = lob.OrderBook(4096)
    rng = random.Random(987654321)

    live: List[int] = []
    trades_seen = 0
    for _ in range(4_000):
        roll = rng.random()

        if roll < 0.15 and live:
            order_id = live.pop(rng.randrange(len(live)))
            book.modify_order(order_id=order_id, new_quantity=rng.randint(0, 200))
        elif roll < 0.35 and live:
            order_id = live.pop(rng.randrange(len(live)))
            # False is legitimate here: the order may have been filled by a
            # trade since it was tracked. It must then be gone from the book.
            if not book.cancel_order(order_id=order_id):
                assert not book.contains(order_id)
        else:
            side = Bid if rng.random() < 0.5 else Ask
            price = round(rng.uniform(98.0, 102.0), 2)
            order_id = book.add_limit_order(
                side=side, price=price, quantity=rng.randint(1, 100)
            )
            live.append(order_id)

        trades_seen += len(book.match())

        # The self-check recomputes every counter, walks both sides in order,
        # and verifies the id index points at the right node.
        problem = book.validate()
        assert problem == "", f"book went inconsistent: {problem}"

    assert trades_seen > 0, "the fuzz workload never crossed -- test is not exercising matching"
    assert book.total_orders() == book.bid_depth() + book.ask_depth()


def test_stress_cancelling_every_order_empties_the_index():
    book = lob.OrderBook(2048)
    ids = [
        book.add_limit_order(side=Bid if i % 2 == 0 else Ask,
                             price=99.0 - i * 0.01 if i % 2 == 0 else 101.0 + i * 0.01,
                             quantity=10)
        for i in range(2_000)
    ]

    for order_id in ids:
        assert book.cancel_order(order_id=order_id) is True

    assert book.empty()
    assert book.bid_levels() == 0 and book.ask_levels() == 0
    assert book.validate() == ""


def test_deep_book_matching_through_many_levels():
    """One aggressive order sweeping 200 levels must produce 200 trades."""
    book = lob.OrderBook(1024)
    for i in range(200):
        book.add_limit_order(side=Ask, price=100.0 + i * 0.01, quantity=10)
    book.match()

    book.add_limit_order(side=Bid, price=102.0, quantity=2_000)
    trades = book.match()

    assert len(trades) == 200
    assert sum(t.quantity for t in trades) == 2_000
    assert [t.price for t in trades] == sorted(t.price for t in trades)
    assert book.empty()
    assert book.validate() == ""
