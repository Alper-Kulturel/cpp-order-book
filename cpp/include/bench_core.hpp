// bench_core.hpp -- latency / throughput harness for the order book.
//
// Header-only so the exact same measurement code backs both the native
// benchmark binary (lob_bench) and the Python-facing bench_driver module; the
// two can never drift apart.
//
// Two things this harness is careful about, because both are easy ways to
// publish a flattering but meaningless number:
//
//   1. Clock cost. now_ns() costs ~20-25ns on Apple silicon, which is a large
//      fraction of an add. A real feed handler stamps orders from the wire, so
//      the benchmark does the same and passes explicit timestamps in. The
//      book's own clock is therefore never on the measured path.
//   2. Setup cost. Populating the book is never inside a timed region; only
//      the operation under test is.

#ifndef LOB_BENCH_CORE_HPP
#define LOB_BENCH_CORE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "order_book.hpp"

namespace lob {
namespace bench {

using Clock = std::chrono::steady_clock;

struct Stats {
    std::size_t ops{0};
    double total_ns{0.0};
    double ns_per_op{0.0};
    double ops_per_sec{0.0};
    std::size_t trades{0};
};

inline Stats make_stats(std::size_t ops, double total_ns, std::size_t trades = 0) {
    Stats s;
    s.ops = ops;
    s.total_ns = total_ns;
    s.ns_per_op = ops > 0 ? total_ns / static_cast<double>(ops) : 0.0;
    s.ops_per_sec = total_ns > 0.0 ? static_cast<double>(ops) * 1e9 / total_ns : 0.0;
    s.trades = trades;
    return s;
}

inline double elapsed_ns(Clock::time_point start, Clock::time_point stop) {
    return std::chrono::duration<double, std::nano>(stop - start).count();
}

/// xorshift64* -- deterministic, allocation-free, and roughly 1ns per draw, so
/// the generator never shows up in the measurement it is feeding.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept
        : state_(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    /// Uniform in [0, bound); bound must be non-zero.
    std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }

    /// Uniform in [0, 1).
    double unit() noexcept {
        return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
    }

    void shuffle(std::vector<OrderId>& v) noexcept {
        for (std::size_t i = v.size(); i > 1; --i) {
            const std::size_t j = static_cast<std::size_t>(below(i));
            const OrderId tmp = v[i - 1];
            v[i - 1] = v[j];
            v[j] = tmp;
        }
    }

private:
    std::uint64_t state_;
};

namespace detail {

constexpr Price kBasePrice = 100.0;
constexpr Price kTick = 0.01;
constexpr Quantity kOrderQty = 100;
/// A price above any ladder this harness builds, so an aggressive order always
/// crosses no matter how large n gets.
constexpr Price kSweepPrice = 1.0e9;
/// Levels per side in the passive benchmarks. Deep enough that the level maps
/// are non-trivial, shallow enough that they stay cache-friendly.
constexpr std::uint64_t kDepthLevels = 500;

/// Timestamps are synthesised rather than read from the clock; see the header
/// comment. Starting at 1 keeps them clear of OrderBook::kAutoTimestamp.
class SyntheticClock {
public:
    Timestamp tick() noexcept { return ++now_; }

private:
    Timestamp now_{0};
};

inline Price bid_price(std::uint64_t slot) noexcept {
    return kBasePrice - kTick * static_cast<Price>(slot + 1);
}

inline Price ask_price(std::uint64_t slot) noexcept {
    return kBasePrice + kTick * static_cast<Price>(slot + 1);
}

}  // namespace detail

/// Pure passive inserts into a book that never crosses -- the raw add path.
inline Stats bench_add(std::size_t n, std::uint64_t seed) {
    OrderBook book(n);
    Rng rng(seed);
    detail::SyntheticClock clock;
    std::uint64_t sink = 0;

    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t slot = rng.below(detail::kDepthLevels);
        const bool is_bid = (rng.next() & 1u) != 0;
        const Side side = is_bid ? Side::Bid : Side::Ask;
        const Price px = is_bid ? detail::bid_price(slot) : detail::ask_price(slot);
        sink += book.add_limit_order(side, px, detail::kOrderQty, clock.tick());
    }
    const auto stop = Clock::now();

    // Keep the optimiser from proving the loop away.
    if (sink == 0xDEADBEEFDEADBEEFull) {
        return make_stats(0, 1.0);
    }
    return make_stats(n, elapsed_ns(start, stop), book.trades_executed());
}

/// Cancels in random order, so each one is a genuine hash lookup + unlink
/// rather than a cache-hot sequential walk.
inline Stats bench_cancel(std::size_t n, std::uint64_t seed) {
    OrderBook book(n);
    Rng rng(seed);
    detail::SyntheticClock clock;
    std::vector<OrderId> ids;
    ids.reserve(n);

    // Setup -- deliberately outside the timed region.
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t slot = rng.below(detail::kDepthLevels);
        const bool is_bid = (rng.next() & 1u) != 0;
        const Side side = is_bid ? Side::Bid : Side::Ask;
        const Price px = is_bid ? detail::bid_price(slot) : detail::ask_price(slot);
        const OrderId id = book.add_limit_order(side, px, detail::kOrderQty, clock.tick());
        if (id != OrderBook::kInvalidOrderId) {
            ids.push_back(id);
        }
    }
    rng.shuffle(ids);

    std::size_t cancelled = 0;
    const auto start = Clock::now();
    for (const OrderId id : ids) {
        if (book.cancel_order(id)) {
            ++cancelled;
        }
    }
    const auto stop = Clock::now();

    return make_stats(cancelled, elapsed_ns(start, stop));
}

/// Aggressive orders that each sweep the best resting level. Resting size is
/// chosen to equal the incoming size, so every aggressor produces exactly one
/// trade and leaves no residue -- this isolates the matching path.
inline Stats bench_match(std::size_t n, std::uint64_t seed) {
    OrderBook book(n);
    detail::SyntheticClock clock;

    // Setup: n resting sells, one order per price level.
    for (std::size_t i = 0; i < n; ++i) {
        book.add_limit_order(Side::Ask, detail::kBasePrice + detail::kTick * static_cast<Price>(i + 1),
                             detail::kOrderQty, clock.tick());
    }

    const std::size_t before = book.trades_executed();
    std::size_t matched = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        // Priced far above the whole ladder so it always crosses, and sized to
        // match the resting orders exactly so it never leaves a remainder.
        // (A merely "high" price silently stops crossing once the ladder
        // climbs past it, which shows up as missing trades.)
        const OrderId id = book.add_limit_order(Side::Bid, detail::kSweepPrice,
                                                detail::kOrderQty, clock.tick());
        if (id != OrderBook::kInvalidOrderId) {
            ++matched;
        }
    }
    const auto stop = Clock::now();

    (void)seed;
    return make_stats(matched, elapsed_ns(start, stop), book.trades_executed() - before);
}

/// The realistic one: a live book churning with a market-like mix of passive
/// adds, cancels of random live orders, and aggressive orders that trade.
/// 70 / 20 / 10 by weight.
inline Stats bench_mixed(std::size_t n, std::uint64_t seed) {
    OrderBook book(n);
    Rng rng(seed);
    detail::SyntheticClock clock;
    std::vector<OrderId> live;
    live.reserve(n);

    std::uint64_t weight = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t roll = rng.below(100);

        if (roll < 70) {
            const std::uint64_t slot = rng.below(detail::kDepthLevels);
            const bool is_bid = (rng.next() & 1u) != 0;
            const Side side = is_bid ? Side::Bid : Side::Ask;
            const Price px = is_bid ? detail::bid_price(slot) : detail::ask_price(slot);
            const OrderId id = book.add_limit_order(side, px, detail::kOrderQty, clock.tick());
            if (id != OrderBook::kInvalidOrderId) {
                live.push_back(id);
            }
        } else if (roll < 90) {
            if (!live.empty()) {
                const std::size_t k = static_cast<std::size_t>(rng.below(live.size()));
                const OrderId id = live[k];
                book.cancel_order(id);
                live[k] = live.back();
                live.pop_back();
            }
        } else {
            // Aggressive: crosses the spread and lifts whatever is there.
            const bool is_bid = (rng.next() & 1u) != 0;
            const Side side = is_bid ? Side::Bid : Side::Ask;
            const Price px = is_bid ? detail::ask_price(0) : detail::bid_price(0);
            const OrderId id = book.add_limit_order(side, px, detail::kOrderQty, clock.tick());
            if (id != OrderBook::kInvalidOrderId) {
                live.push_back(id);
            }
        }
        weight += roll;
    }
    const auto stop = Clock::now();

    if (weight == 0xFFFFFFFFull) {
        return make_stats(0, 1.0);
    }
    return make_stats(n, elapsed_ns(start, stop), book.trades_executed());
}

/// Cost of a single now_ns() call, so the README can state how much of an
/// operation the clock would have accounted for.
inline Stats bench_clock(std::size_t n, std::uint64_t seed) {
    (void)seed;
    Timestamp sink = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        sink += now_ns();
    }
    const auto stop = Clock::now();
    if (sink == 0 && n != 0) {
        return make_stats(0, 1.0);
    }
    return make_stats(n, elapsed_ns(start, stop));
}

}  // namespace bench
}  // namespace lob

#endif  // LOB_BENCH_CORE_HPP
