// bench_driver.cpp -- pybind11 module wrapping the benchmark harness.
//
// Exists so benchmarks/benchmark.py measures the C++ book rather than the
// Python interpreter: a 1,000,000-iteration loop driven from Python would
// spend most of its time in the interpreter and the pybind11 trampoline, and
// would report that as "order book latency".
//
// The GIL is released once for the whole run, which is both correct (the loop
// touches no Python state) and necessary (the alternative is ~50-100ns of GIL
// handoff per call, which is the same order as the operation being measured).

#include <pybind11/pybind11.h>

#include <cstddef>
#include <cstdint>

#include "bench_core.hpp"

namespace py = pybind11;
namespace bench = lob::bench;

namespace {

py::dict to_dict(const bench::Stats& s) {
    py::dict d;
    d["ops"] = s.ops;
    d["total_ns"] = s.total_ns;
    d["ns_per_op"] = s.ns_per_op;
    d["ops_per_sec"] = s.ops_per_sec;
    d["trades"] = s.trades;
    return d;
}

/// Run a harness function with the GIL released for the duration.
template <typename Fn>
py::dict timed(Fn&& fn) {
    bench::Stats stats;
    {
        py::gil_scoped_release release;
        stats = fn();
    }
    return to_dict(stats);
}

}  // namespace

PYBIND11_MODULE(bench_driver, m) {
    m.doc() = R"doc(
Native benchmark harness for order_book_cpp.

Every function releases the GIL for the duration of its run, so the numbers
reflect the C++ order book and not the Python interpreter.
)doc";

    m.attr("__version__") = "1.0.0";

    m.def(
        "bench_add",
        [](std::size_t n, std::uint64_t seed) {
            return timed([&] { return bench::bench_add(n, seed); });
        },
        py::arg("n"), py::arg("seed") = 42,
        "Passive inserts into a non-crossing book. Returns ops, total_ns, ns_per_op, "
        "ops_per_sec, trades.");

    m.def(
        "bench_cancel",
        [](std::size_t n, std::uint64_t seed) {
            return timed([&] { return bench::bench_cancel(n, seed); });
        },
        py::arg("n"), py::arg("seed") = 42,
        "Random-order cancels of n resting orders (setup is excluded). Returns the same keys.");

    m.def(
        "bench_match",
        [](std::size_t n, std::uint64_t seed) {
            return timed([&] { return bench::bench_match(n, seed); });
        },
        py::arg("n"), py::arg("seed") = 42,
        "Aggressive orders that sweep the best resting level. Returns the same keys.");

    m.def(
        "bench_mixed",
        [](std::size_t n, std::uint64_t seed) {
            return timed([&] { return bench::bench_mixed(n, seed); });
        },
        py::arg("n"), py::arg("seed") = 42,
        "Realistic 70/20/10 add/cancel/aggressive mix. Returns the same keys.");

    m.def(
        "bench_clock",
        [](std::size_t n, std::uint64_t seed) {
            return timed([&] { return bench::bench_clock(n, seed); });
        },
        py::arg("n") = 1000000, py::arg("seed") = 42,
        "Cost of one now_ns() call, i.e. what the book saves by having the caller "
        "supply feed timestamps.");

    m.def(
        "bench_suite",
        [](std::size_t n, std::uint64_t seed) {
            py::dict out;
            out["add"] = timed([&] { return bench::bench_add(n, seed); });
            out["cancel"] = timed([&] { return bench::bench_cancel(n, seed); });
            out["match"] = timed([&] { return bench::bench_match(n, seed); });
            out["mixed"] = timed([&] { return bench::bench_mixed(n, seed); });
            out["clock"] = timed([&] { return bench::bench_clock(n, seed); });
            out["n"] = n;
            out["seed"] = seed;
            return out;
        },
        py::arg("n") = 1000000, py::arg("seed") = 42,
        "Run the whole harness at size n in one call, releasing the GIL throughout.");
}
