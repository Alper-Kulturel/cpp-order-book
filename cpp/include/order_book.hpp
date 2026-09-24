// order_book.hpp -- public API of a price/time-priority limit order book.
//
// Design goals
//   * Price/time priority (FIFO inside a price level), the matching discipline
//     used by essentially every cash equity and futures venue.
//   * add_limit_order  : O(log L) level lookup + O(1) append, L = #price levels
//   * cancel_order     : O(1) -- hash lookup, then erase by stored iterator
//   * best_bid/best_ask: O(1) -- the level maps are ordered best-first
//   * No exceptions on the hot path; invalid input is reported by return value.
//
// THREAD SAFETY: this class is deliberately NOT thread-safe. It is a
// single-writer data structure -- the classic exchange gateway / market-making
// hot path is one thread mutating the book and zero locks. Callers that need
// concurrency must serialise externally (or shard by instrument).
//
// MEMORY: all book storage lives in one std::pmr pool owned by the book, which
// removes the general-purpose allocator from the add/cancel path. See
// docs/architecture.md for measurements.

#ifndef LOB_ORDER_BOOK_HPP
#define LOB_ORDER_BOOK_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lob {

/// Which side of the book an order rests on.
enum class Side : std::uint8_t {
    Bid = 0,  ///< buy
    Ask = 1,  ///< sell
};

using OrderId = std::uint64_t;
using Quantity = std::uint64_t;
using Price = double;
/// Nanoseconds. Supplied by the caller when replaying a feed; otherwise taken
/// from a monotonic clock at insertion time.
using Timestamp = std::uint64_t;

/// A resting (passive) limit order.
struct Order {
    OrderId id;
    Price price;
    Quantity quantity;  ///< remaining (unfilled) quantity
    Side side;
    Timestamp timestamp;
};

/// A completed fill between a resting buy order and a resting sell order.
struct Trade {
    OrderId buy_order_id;
    OrderId sell_order_id;
    Price price;  ///< the passive order's price (price improvement goes to the taker)
    Quantity quantity;
    Timestamp timestamp;
};

/// Monotonic clock reading in nanoseconds, used when the caller does not
/// supply a timestamp. Never goes backwards; not wall-clock.
Timestamp now_ns() noexcept;

/// Price/time-priority limit order book for a single instrument.
///
/// Orders are matched on entry (continuous trading): adding an order that
/// crosses the spread fills it immediately against the resting book, and the
/// resulting fills are buffered until retrieved with match().
///
/// Not copyable. Move-constructible; move-assignment is deleted (see below).
class OrderBook {
public:
    /// FIFO queue of resting orders at one price. std::list gives O(1) splice
    /// and erase from the middle, which is what a cancel needs.
    using PriceLevel = std::pmr::list<Order>;
    /// Bids, best (highest) price first.
    using BidLevels = std::pmr::map<Price, PriceLevel, std::greater<Price>>;
    /// Asks, best (lowest) price first.
    using AskLevels = std::pmr::map<Price, PriceLevel, std::less<Price>>;
    /// (price, total resting quantity) pairs, best price first.
    using LevelDepth = std::vector<std::pair<Price, Quantity>>;

    /// Sentinel meaning "stamp this order with now_ns()".
    static constexpr Timestamp kAutoTimestamp = 0;
    /// Returned by add_limit_order when the request was rejected.
    static constexpr OrderId kInvalidOrderId = 0;

    /// @param expected_orders pre-sizes the id index; purely a hint.
    explicit OrderBook(std::size_t expected_orders = 1u << 16);
    ~OrderBook();

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    /// Move construction is O(1): the containers adopt the source's pool.
    OrderBook(OrderBook&& other) = default;
    /// Deleted on purpose. The containers are pmr containers holding a raw
    /// pointer to pool_, and polymorphic_allocator propagates on move
    /// assignment -- a member-wise move-assign would therefore destroy our pool
    /// before the containers had returned their blocks to it. Construct a new
    /// book, or call clear(), instead.
    OrderBook& operator=(OrderBook&& other) = delete;

    // ---------------------------------------------------------------- mutators

    /// Insert a limit order, then match it against the resting book.
    ///
    /// @param  side       Bid or Ask.
    /// @param  price      limit price; must be finite and > 0.
    /// @param  quantity   must be > 0.
    /// @param  timestamp  feed timestamp, or kAutoTimestamp to stamp on entry.
    /// @return the new order id, or kInvalidOrderId if the request was rejected.
    ///
    /// A crossing order fills immediately at the resting orders' prices and can
    /// generate several trades (it walks the book). Any unfilled remainder
    /// rests. Trades are buffered; retrieve them with match().
    ///
    /// Caller contract: timestamps should be non-decreasing across calls. Every
    /// real feed satisfies this, and validate() relies on it to verify that
    /// each price level is genuinely in time-priority order.
    OrderId add_limit_order(Side side, Price price, Quantity quantity,
                            Timestamp timestamp = kAutoTimestamp);

    /// Remove a resting order.
    /// @return true if the order existed and was removed, false otherwise
    ///         (unknown id, or already filled).
    bool cancel_order(OrderId order_id);

    /// Change the remaining quantity of a resting order.
    ///
    /// Exchange semantics: a size *reduction* keeps the order's time priority;
    /// a size *increase* sends it to the back of its price level, because it
    /// could otherwise jump the queue. new_quantity == 0 cancels the order.
    ///
    /// An order that loses priority is re-stamped, so its timestamp always
    /// describes the queue position it currently holds rather than when it was
    /// first submitted. This is what a venue does when it treats a
    /// priority-losing replace as a fresh queue entry.
    ///
    /// @param  timestamp  re-stamp value for a priority-losing increase, or
    ///                    kAutoTimestamp to take one from the clock.
    /// @return true if the order existed and was modified/cancelled.
    bool modify_order(OrderId order_id, Quantity new_quantity,
                      Timestamp timestamp = kAutoTimestamp);

    /// Return the trades executed since the previous call and clear the buffer.
    ///
    /// Because orders match on entry, this normally returns the fills produced
    /// by the add_limit_order calls made since the last drain. It also sweeps
    /// any residual cross first, so it is safe to call at any time.
    std::vector<Trade> match();

    /// Drop every order, trade and counter, keeping the allocated pool.
    void clear();

    // --------------------------------------------------------------- accessors

    /// Best (highest) bid, or NaN when there are no bids.
    [[nodiscard]] Price best_bid() const noexcept;
    /// Best (lowest) ask, or NaN when there are no asks.
    [[nodiscard]] Price best_ask() const noexcept;
    /// best_ask - best_bid, or NaN if either side is empty.
    [[nodiscard]] Price spread() const noexcept;
    /// (best_bid + best_ask) / 2, or NaN if either side is empty.
    [[nodiscard]] Price mid_price() const noexcept;

    /// Number of resting orders on the bid side.
    [[nodiscard]] std::size_t bid_depth() const noexcept;
    /// Number of resting orders on the ask side.
    [[nodiscard]] std::size_t ask_depth() const noexcept;
    /// Total resting orders on both sides.
    [[nodiscard]] std::size_t total_orders() const noexcept;
    /// Number of distinct bid price levels.
    [[nodiscard]] std::size_t bid_levels() const noexcept;
    /// Number of distinct ask price levels.
    [[nodiscard]] std::size_t ask_levels() const noexcept;
    /// Total resting quantity on the bid side.
    [[nodiscard]] Quantity bid_volume() const noexcept;
    /// Total resting quantity on the ask side.
    [[nodiscard]] Quantity ask_volume() const noexcept;

    [[nodiscard]] bool has_bids() const noexcept { return !bids_.empty(); }
    [[nodiscard]] bool has_asks() const noexcept { return !asks_.empty(); }
    [[nodiscard]] bool empty() const noexcept { return bids_.empty() && asks_.empty(); }

    /// Best n bid levels as (price, total quantity), highest price first.
    [[nodiscard]] LevelDepth top_n_bids(std::size_t n) const;
    /// Best n ask levels as (price, total quantity), lowest price first.
    [[nodiscard]] LevelDepth top_n_asks(std::size_t n) const;

    /// Resting orders at one price, in queue (time priority) order -- front of
    /// the queue first. Empty if nothing rests there.
    ///
    /// O(k) in the number of orders at that level. This is introspective, for
    /// analytics and tests; it is not a hot-path call.
    [[nodiscard]] std::vector<Order> level_orders(Side side, Price price) const;

    /// Look up a resting order without copying it; nullptr if not resting.
    [[nodiscard]] const Order* find_order(OrderId order_id) const noexcept;
    /// True if the order is currently resting on the book.
    [[nodiscard]] bool contains(OrderId order_id) const noexcept;

    // -------------------------------------------------------------- statistics

    [[nodiscard]] std::size_t trades_executed() const noexcept { return trades_executed_; }
    [[nodiscard]] Quantity volume_traded() const noexcept { return volume_traded_; }
    [[nodiscard]] Price last_trade_price() const noexcept { return last_trade_price_; }
    [[nodiscard]] OrderId last_order_id() const noexcept { return next_order_id_ - 1; }

    /// Recompute every derived quantity from first principles and compare it
    /// with the incrementally maintained state. Returns an empty string when
    /// the book is consistent, otherwise a description of the first problem.
    ///
    /// This is an O(n) debug/CI aid, never call it on the hot path.
    [[nodiscard]] std::string validate() const;

    /// Human-readable one-line summary, e.g. "bids=3 asks=4 trades=2".
    [[nodiscard]] std::string to_string() const;

private:
    /// Where a resting order lives, so it can be cancelled in O(1).
    /// Both level iterators are kept even though only one is live: the bid and
    /// ask maps have different comparators and therefore different iterator
    /// types, and 8 wasted bytes per resting order is cheaper than a branchy
    /// type-erased alternative on the cancel path.
    struct OrderLocation {
        Side side;
        BidLevels::iterator bid_level;
        AskLevels::iterator ask_level;
        PriceLevel::iterator order;
    };

    using OrderIndex = std::pmr::unordered_map<OrderId, OrderLocation>;

    // Fills the top of both books; returns the trade it created.
    Trade match_top_(Price trade_price);

    // Consume a crossing book after an aggressive add on `aggressor_side`,
    // trading at the resting side's price.
    void match_aggressive_(Side aggressor_side);

    // Safety net for a crossed book (should be unreachable via the public API).
    void sweep_cross_();

    // Unlink a resting order from its level, dropping the level if it empties.
    template <typename LevelMap>
    void unlink_(LevelMap& levels, typename LevelMap::iterator level_it,
                 typename PriceLevel::iterator order_it);

    [[nodiscard]] PriceLevel& level_of_(const OrderLocation& loc) noexcept {
        return loc.side == Side::Bid ? loc.bid_level->second : loc.ask_level->second;
    }

    // Declared first so it is destroyed last: the containers below hand their
    // blocks back to it during their own destruction.
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> pool_;
    BidLevels bids_;
    AskLevels asks_;
    OrderIndex index_;

    OrderId next_order_id_{1};  ///< ids are 1-based; 0 is the invalid sentinel
    std::size_t bid_order_count_{0};
    std::size_t ask_order_count_{0};
    Quantity bid_volume_{0};
    Quantity ask_volume_{0};

    std::size_t trades_executed_{0};
    Quantity volume_traded_{0};
    Price last_trade_price_{0.0};
    std::vector<Trade> pending_trades_;
};

}  // namespace lob

#endif  // LOB_ORDER_BOOK_HPP
