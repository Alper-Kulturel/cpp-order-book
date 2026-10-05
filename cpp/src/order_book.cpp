#include "order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>

namespace lob {
namespace {

constexpr Price kNaN = std::numeric_limits<Price>::quiet_NaN();
constexpr Price kInf = std::numeric_limits<Price>::infinity();

inline bool is_valid_price(Price p) noexcept {
    return std::isfinite(p) && p > 0.0;
}

std::string format_price(Price p) {
    if (std::isnan(p)) return "-";
    std::ostringstream os;
    os << p;
    return os.str();
}

}  // namespace

Timestamp now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<Timestamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

OrderBook::OrderBook(std::size_t expected_orders)
    : pool_(std::make_unique<std::pmr::unsynchronized_pool_resource>()),
      bids_(std::greater<Price>{}, pool_.get()),
      asks_(std::less<Price>{}, pool_.get()),
      index_(pool_.get()) {
    if (expected_orders > 0) {
        // Pre-size the bucket array so the first few hundred thousand inserts
        // do not trigger rehashes. The pool needs no equivalent warm-up: it
        // grows in chunks and recycles freed nodes.
        index_.reserve(expected_orders);
    }
}

OrderBook::~OrderBook() = default;

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

OrderId OrderBook::add_limit_order(Side side, Price price, Quantity quantity,
                                   Timestamp timestamp) {
    if (quantity == 0 || !is_valid_price(price)) {
        return kInvalidOrderId;
    }

    const OrderId id = next_order_id_++;
    const Timestamp ts = (timestamp == kAutoTimestamp) ? now_ns() : timestamp;

    if (side == Side::Bid) {
        auto level_it = bids_.try_emplace(price).first;
        PriceLevel& level = level_it->second;
        level.push_back(Order{id, price, quantity, side, ts});
        index_.emplace(
            id, OrderLocation{side, level_it, AskLevels::iterator{}, std::prev(level.end())});
        ++bid_order_count_;
        bid_volume_ += quantity;
    } else {
        auto level_it = asks_.try_emplace(price).first;
        PriceLevel& level = level_it->second;
        level.push_back(Order{id, price, quantity, side, ts});
        index_.emplace(
            id, OrderLocation{side, BidLevels::iterator{}, level_it, std::prev(level.end())});
        ++ask_order_count_;
        ask_volume_ += quantity;
    }

    match_aggressive_(side, ts);
    return id;
}

bool OrderBook::cancel_order(OrderId order_id) {
    auto it = index_.find(order_id);
    if (it == index_.end()) {
        return false;
    }
    // Copy the location: unlink_ erases from index_ and invalidates `it`.
    const OrderLocation loc = it->second;
    if (loc.side == Side::Bid) {
        unlink_(bids_, loc.bid_level, loc.order);
    } else {
        unlink_(asks_, loc.ask_level, loc.order);
    }
    return true;
}

bool OrderBook::modify_order(OrderId order_id, Quantity new_quantity, Timestamp timestamp) {
    auto it = index_.find(order_id);
    if (it == index_.end()) {
        return false;
    }
    if (new_quantity == 0) {
        return cancel_order(order_id);
    }

    OrderLocation& loc = it->second;
    Order& order = *loc.order;
    const bool is_bid = (loc.side == Side::Bid);

    if (new_quantity <= order.quantity) {
        // Reducing size cannot change who deserves to trade first, so the
        // order keeps its place in the queue.
        const Quantity delta = order.quantity - new_quantity;
        order.quantity = new_quantity;
        if (is_bid) {
            bid_volume_ -= delta;
        } else {
            ask_volume_ -= delta;
        }
        return true;
    }

    // Increasing size could let the order jump ahead of traders who queued
    // before it, so the exchange convention is to re-queue it at the back of
    // its level. splice() relinks the existing node rather than reallocating,
    // and list iterators survive splicing, so loc.order stays valid.
    const Quantity delta = new_quantity - order.quantity;
    order.quantity = new_quantity;
    if (is_bid) {
        bid_volume_ += delta;
    } else {
        ask_volume_ += delta;
    }
    PriceLevel& level = level_of_(loc);
    level.splice(level.end(), level, loc.order);
    // The order now sits at the back of the queue, so its timestamp has to
    // advance with it -- otherwise it would still carry the arrival time of
    // the queue position it just gave up, and the level would no longer read
    // as time-ordered.
    order.timestamp = (timestamp == kAutoTimestamp) ? now_ns() : timestamp;
    return true;
}

std::vector<Trade> OrderBook::match() {
    sweep_cross_();
    std::vector<Trade> out;
    out.swap(pending_trades_);
    return out;
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    index_.clear();
    pending_trades_.clear();
    next_order_id_ = 1;
    bid_order_count_ = 0;
    ask_order_count_ = 0;
    bid_volume_ = 0;
    ask_volume_ = 0;
    trades_executed_ = 0;
    volume_traded_ = 0;
    last_trade_price_ = 0.0;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

Trade OrderBook::match_top_(Price trade_price, Timestamp timestamp) {
    auto bid_level = bids_.begin();
    auto ask_level = asks_.begin();

    Order& buy = bid_level->second.front();
    Order& sell = ask_level->second.front();

    const Quantity qty = std::min(buy.quantity, sell.quantity);

    buy.quantity -= qty;
    sell.quantity -= qty;
    bid_volume_ -= qty;
    ask_volume_ -= qty;

    const Trade trade{buy.id, sell.id, trade_price, qty, timestamp};

    // Fully filled orders leave the book. unlink_ may erase a price level, but
    // the two levels live in different maps, so the sibling iterator stays
    // valid and the second check below is safe.
    if (buy.quantity == 0) {
        unlink_(bids_, bid_level, bid_level->second.begin());
    }
    if (sell.quantity == 0) {
        unlink_(asks_, ask_level, ask_level->second.begin());
    }

    ++trades_executed_;
    volume_traded_ += qty;
    last_trade_price_ = trade_price;
    pending_trades_.push_back(trade);
    return trade;
}

void OrderBook::match_aggressive_(Side aggressor_side, Timestamp aggressor_ts) {
    // The book is never crossed on entry, so any cross here involves the order
    // we just added and the price it pays is the resting side's price.
    while (!bids_.empty() && !asks_.empty()) {
        const auto bid_level = bids_.begin();
        const auto ask_level = asks_.begin();
        if (bid_level->first < ask_level->first) {
            return;  // spread intact, nothing to do
        }
        const Price px = (aggressor_side == Side::Bid) ? ask_level->first : bid_level->first;
        match_top_(px, aggressor_ts);
    }
}

void OrderBook::sweep_cross_() {
    while (!bids_.empty() && !asks_.empty()) {
        const auto bid_level = bids_.begin();
        const auto ask_level = asks_.begin();
        if (bid_level->first < ask_level->first) {
            return;
        }
        // No aggressor is known here, so the order that was resting longer is
        // treated as the passive one and sets the price. Both prices and the
        // timestamps are read before match_top_ invalidates these references.
        // The cross exists from the moment the second of the two orders
        // arrived, so the fill carries the later of the two stamps.
        const Order& buy = bid_level->second.front();
        const Order& sell = ask_level->second.front();
        const bool buy_is_passive = buy.timestamp <= sell.timestamp;
        match_top_(buy_is_passive ? buy.price : sell.price,
                   std::max(buy.timestamp, sell.timestamp));
    }
}

template <typename LevelMap>
void OrderBook::unlink_(LevelMap& levels, typename LevelMap::iterator level_it,
                        typename PriceLevel::iterator order_it) {
    const Order& order = *order_it;
    if (order.side == Side::Bid) {
        --bid_order_count_;
        bid_volume_ -= order.quantity;
    } else {
        --ask_order_count_;
        ask_volume_ -= order.quantity;
    }
    index_.erase(order.id);
    level_it->second.erase(order_it);
    if (level_it->second.empty()) {
        levels.erase(level_it);
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Price OrderBook::best_bid() const noexcept {
    return bids_.empty() ? kNaN : bids_.begin()->first;
}

Price OrderBook::best_ask() const noexcept {
    return asks_.empty() ? kNaN : asks_.begin()->first;
}

Price OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) return kNaN;
    return asks_.begin()->first - bids_.begin()->first;
}

Price OrderBook::mid_price() const noexcept {
    if (bids_.empty() || asks_.empty()) return kNaN;
    return (bids_.begin()->first + asks_.begin()->first) * 0.5;
}

std::size_t OrderBook::bid_depth() const noexcept { return bid_order_count_; }
std::size_t OrderBook::ask_depth() const noexcept { return ask_order_count_; }

std::size_t OrderBook::total_orders() const noexcept {
    return bid_order_count_ + ask_order_count_;
}

std::size_t OrderBook::bid_levels() const noexcept { return bids_.size(); }
std::size_t OrderBook::ask_levels() const noexcept { return asks_.size(); }
Quantity OrderBook::bid_volume() const noexcept { return bid_volume_; }
Quantity OrderBook::ask_volume() const noexcept { return ask_volume_; }

OrderBook::LevelDepth OrderBook::top_n_bids(std::size_t n) const {
    LevelDepth out;
    out.reserve(std::min(n, bids_.size()));
    for (auto it = bids_.begin(); it != bids_.end() && out.size() < n; ++it) {
        Quantity total = 0;
        for (const Order& o : it->second) total += o.quantity;
        out.emplace_back(it->first, total);
    }
    return out;
}

OrderBook::LevelDepth OrderBook::top_n_asks(std::size_t n) const {
    LevelDepth out;
    out.reserve(std::min(n, asks_.size()));
    for (auto it = asks_.begin(); it != asks_.end() && out.size() < n; ++it) {
        Quantity total = 0;
        for (const Order& o : it->second) total += o.quantity;
        out.emplace_back(it->first, total);
    }
    return out;
}

std::vector<Order> OrderBook::level_orders(Side side, Price price) const {
    std::vector<Order> out;
    if (side == Side::Bid) {
        auto it = bids_.find(price);
        if (it == bids_.end()) return out;
        out.assign(it->second.begin(), it->second.end());
    } else {
        auto it = asks_.find(price);
        if (it == asks_.end()) return out;
        out.assign(it->second.begin(), it->second.end());
    }
    return out;
}

const Order* OrderBook::find_order(OrderId order_id) const noexcept {
    auto it = index_.find(order_id);
    return it == index_.end() ? nullptr : &*it->second.order;
}

bool OrderBook::contains(OrderId order_id) const noexcept {
    return index_.find(order_id) != index_.end();
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------

std::string OrderBook::validate() const {
    std::size_t bid_count = 0;
    std::size_t ask_count = 0;
    Quantity bid_vol = 0;
    Quantity ask_vol = 0;
    std::size_t indexed = 0;

    Price prev_price = kInf;
    for (auto level = bids_.begin(); level != bids_.end(); ++level) {
        if (level->second.empty()) return "empty bid price level at " + format_price(level->first);
        if (!is_valid_price(level->first)) return "invalid bid price";
        if (level->first >= prev_price) return "bid levels are not strictly descending";
        prev_price = level->first;

        Timestamp prev_ts = 0;
        for (const Order& o : level->second) {
            if (o.side != Side::Bid) return "ask resting in the bid book";
            if (o.price != level->first) return "bid order price disagrees with its level";
            if (o.quantity == 0) return "zero-quantity bid resting on the book";
            if (o.timestamp < prev_ts) return "bid level is not in time priority";
            prev_ts = o.timestamp;

            auto ix = index_.find(o.id);
            if (ix == index_.end()) return "bid order missing from the id index";
            if (ix->second.side != Side::Bid) return "index says this bid is an ask";
            if (&*ix->second.order != &o) return "index points at the wrong bid order";
            ++bid_count;
            bid_vol += o.quantity;
            ++indexed;
        }
    }

    prev_price = 0.0;
    for (auto level = asks_.begin(); level != asks_.end(); ++level) {
        if (level->second.empty()) return "empty ask price level at " + format_price(level->first);
        if (!is_valid_price(level->first)) return "invalid ask price";
        if (level->first <= prev_price) return "ask levels are not strictly ascending";
        prev_price = level->first;

        Timestamp prev_ts = 0;
        for (const Order& o : level->second) {
            if (o.side != Side::Ask) return "bid resting in the ask book";
            if (o.price != level->first) return "ask order price disagrees with its level";
            if (o.quantity == 0) return "zero-quantity ask resting on the book";
            if (o.timestamp < prev_ts) return "ask level is not in time priority";
            prev_ts = o.timestamp;

            auto ix = index_.find(o.id);
            if (ix == index_.end()) return "ask order missing from the id index";
            if (ix->second.side != Side::Ask) return "index says this ask is a bid";
            if (&*ix->second.order != &o) return "index points at the wrong ask order";
            ++ask_count;
            ask_vol += o.quantity;
            ++indexed;
        }
    }

    if (!bids_.empty() && !asks_.empty() && !(bids_.begin()->first < asks_.begin()->first)) {
        return "book is crossed: best bid " + format_price(bids_.begin()->first) +
               " >= best ask " + format_price(asks_.begin()->first);
    }
    if (bid_count != bid_order_count_) return "bid_order_count_ drifted from the book";
    if (ask_count != ask_order_count_) return "ask_order_count_ drifted from the book";
    if (bid_vol != bid_volume_) return "bid_volume_ drifted from the book";
    if (ask_vol != ask_volume_) return "ask_volume_ drifted from the book";
    if (indexed != index_.size()) return "duplicate or stale entries in the id index";
    return std::string{};
}

std::string OrderBook::to_string() const {
    std::ostringstream os;
    os << "OrderBook{best_bid=" << format_price(best_bid())
       << ", best_ask=" << format_price(best_ask())
       << ", spread=" << format_price(spread())
       << ", mid=" << format_price(mid_price())
       << ", bid_depth=" << bid_order_count_ << " over " << bids_.size() << " levels"
       << ", ask_depth=" << ask_order_count_ << " over " << asks_.size() << " levels"
       << ", trades=" << trades_executed_ << "}";
    return os.str();
}

}  // namespace lob
