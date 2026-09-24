// bindings.cpp -- pybind11 module exposing the order book to Python.
//
// The module is a thin, zero-copy-where-possible wrapper: every method maps
// one-to-one onto the C++ API, std::vector results convert to Python lists
// automatically, and keyword arguments mirror the C++ parameter names.
//
// The GIL is intentionally NOT released per call. Releasing and reacquiring it
// costs more than an add does, so doing it per order would make the book look
// slower than it is. Bulk operations that are genuinely long (the benchmark
// drivers) release the GIL once for the whole run instead.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <string>

#include "order_book.hpp"

namespace py = pybind11;
using lob::Order;
using lob::OrderBook;
using lob::OrderId;
using lob::Price;
using lob::Quantity;
using lob::Side;
using lob::Timestamp;

PYBIND11_MODULE(order_book_cpp, m) {
    m.doc() = R"doc(
Price/time-priority limit order book.

Orders match on entry (continuous trading): adding an order that crosses the
spread fills it immediately against the resting book and any unfilled remainder
rests. Fills are buffered and retrieved with OrderBook.match().

The book is NOT thread-safe. It is a single-writer structure -- drive it from
one thread, as an exchange gateway or market-making strategy would.
)doc";

    m.attr("__version__") = "1.0.0";
    m.attr("INVALID_ORDER_ID") = OrderBook::kInvalidOrderId;

    m.def("now_ns", &lob::now_ns, "Monotonic clock reading in nanoseconds.");

    py::enum_<Side>(m, "Side", "Which side of the book an order rests on.")
        .value("Bid", Side::Bid, "Buy side; ordered highest price first.")
        .value("Ask", Side::Ask, "Sell side; ordered lowest price first.")
        .export_values();

    py::class_<Order>(m, "Order", "A resting (passive) limit order.")
        .def(py::init<>())
        .def_readwrite("id", &lob::Order::id, "Unique order id.")
        .def_readwrite("price", &lob::Order::price, "Limit price.")
        .def_readwrite("quantity", &lob::Order::quantity, "Remaining quantity.")
        .def_readwrite("side", &lob::Order::side, "Side.Bid or Side.Ask.")
        .def_readwrite("timestamp", &lob::Order::timestamp, "Feed timestamp, nanoseconds.")
        .def("__repr__", [](const lob::Order& o) {
            return "<Order id=" + std::to_string(o.id) + " " +
                   (o.side == Side::Bid ? "Bid" : "Ask") + " price=" + std::to_string(o.price) +
                   " qty=" + std::to_string(o.quantity) + " ts=" + std::to_string(o.timestamp) +
                   ">";
        });

    py::class_<lob::Trade>(m, "Trade", "A completed fill between two resting orders.")
        .def(py::init<>())
        .def_readwrite("buy_order_id", &lob::Trade::buy_order_id, "Resting buy order id.")
        .def_readwrite("sell_order_id", &lob::Trade::sell_order_id, "Resting sell order id.")
        .def_readwrite("price", &lob::Trade::price, "Trade price (the passive side's price).")
        .def_readwrite("quantity", &lob::Trade::quantity, "Executed quantity.")
        .def_readwrite("timestamp", &lob::Trade::timestamp, "Execution timestamp, nanoseconds.")
        .def("__repr__", [](const lob::Trade& t) {
            return "<Trade buy=" + std::to_string(t.buy_order_id) + " sell=" +
                   std::to_string(t.sell_order_id) + " price=" + std::to_string(t.price) +
                   " qty=" + std::to_string(t.quantity) + ">";
        });

    py::class_<OrderBook>(m, "OrderBook", "Price/time-priority limit order book for one instrument.")
        .def(py::init<std::size_t>(), py::arg("expected_orders") = static_cast<std::size_t>(1u << 16),
             "Create an empty book. `expected_orders` pre-sizes the id index.")

        // -- mutators -------------------------------------------------------
        .def("add_limit_order", &OrderBook::add_limit_order, py::arg("side"), py::arg("price"),
             py::arg("quantity"), py::arg("timestamp") = OrderBook::kAutoTimestamp,
             R"doc(Insert a limit order and match it against the resting book.

Returns the new order id, or INVALID_ORDER_ID (0) if the request was rejected
(quantity == 0, or price not finite and positive).

A crossing order walks the book, filling at the resting orders' prices and
producing one trade per resting order it consumes. Any unfilled remainder
rests. Retrieve the fills with match().)doc")
        .def("cancel_order", &OrderBook::cancel_order, py::arg("order_id"),
             "Remove a resting order. True if it was found and removed, False otherwise.")
        .def("modify_order", &OrderBook::modify_order, py::arg("order_id"), py::arg("new_quantity"),
             py::arg("timestamp") = OrderBook::kAutoTimestamp,
             R"doc(Change the remaining quantity of a resting order.

A reduction keeps the order's time priority; an increase re-queues it at the
back of its price level, and re-stamps it so its timestamp tracks the queue
position it now holds. new_quantity == 0 cancels the order. Returns True if the
order existed.)doc")
        .def("match", &OrderBook::match,
             "Return the trades executed since the last call, and clear the buffer.")
        .def("clear", &OrderBook::clear, "Drop all orders, trades and counters, keeping the pool.")

        // -- accessors ------------------------------------------------------
        .def("best_bid", &OrderBook::best_bid, "Best (highest) bid, or NaN if there are none.")
        .def("best_ask", &OrderBook::best_ask, "Best (lowest) ask, or NaN if there are none.")
        .def("spread", &OrderBook::spread, "best_ask - best_bid, or NaN if either side is empty.")
        .def("mid_price", &OrderBook::mid_price, "Midpoint of the spread, or NaN.")
        .def("bid_depth", &OrderBook::bid_depth, "Number of resting orders on the bid side.")
        .def("ask_depth", &OrderBook::ask_depth, "Number of resting orders on the ask side.")
        .def("total_orders", &OrderBook::total_orders, "Resting orders on both sides.")
        .def("bid_levels", &OrderBook::bid_levels, "Number of distinct bid price levels.")
        .def("ask_levels", &OrderBook::ask_levels, "Number of distinct ask price levels.")
        .def("bid_volume", &OrderBook::bid_volume, "Total resting quantity on the bid side.")
        .def("ask_volume", &OrderBook::ask_volume, "Total resting quantity on the ask side.")
        .def("has_bids", &OrderBook::has_bids)
        .def("has_asks", &OrderBook::has_asks)
        .def("empty", &OrderBook::empty)
        .def("top_n_bids", &OrderBook::top_n_bids, py::arg("n"),
             "Best n bid levels as (price, total quantity), highest price first.")
        .def("top_n_asks", &OrderBook::top_n_asks, py::arg("n"),
             "Best n ask levels as (price, total quantity), lowest price first.")
        .def("level_orders", &OrderBook::level_orders, py::arg("side"), py::arg("price"),
             "Resting orders at one price, in queue order (front of the queue first). "
             "Empty if nothing rests there.")
        // `copy`, not the default `automatic`: the Order is owned by the book's
        // pool, so handing Python an owning pointer would double-free it.
        .def("find_order", &OrderBook::find_order, py::arg("order_id"),
             py::return_value_policy::copy,
             "The resting order with this id, or None. The returned object is a copy.")
        .def("contains", &OrderBook::contains, py::arg("order_id"),
             "True if the order is currently resting on the book.")

        // -- statistics -----------------------------------------------------
        .def("trades_executed", &OrderBook::trades_executed, "Trades executed over the book's life.")
        .def("volume_traded", &OrderBook::volume_traded, "Quantity traded over the book's life.")
        .def("last_trade_price", &OrderBook::last_trade_price)
        .def("last_order_id", &OrderBook::last_order_id, "Id of the most recently accepted order.")
        .def("validate", &OrderBook::validate,
             "Self-check: recompute everything and compare. '' means consistent, otherwise the "
             "first problem found. O(n); not for the hot path.")
        .def("to_string", &OrderBook::to_string)

        .def("__len__", &OrderBook::total_orders)
        .def("__repr__", &OrderBook::to_string);
}
