// bench_main.cpp -- standalone native benchmark binary (no Python involved).
//
//   ./build/lob_bench [n] [seed]
//
// The Python harness in benchmarks/benchmark.py reports the same numbers via
// bench_driver; this binary exists so the result can be reproduced with nothing
// but a compiler, and so the C++-only cost is visible.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bench_core.hpp"

namespace {

void print_row(const char* name, const lob::bench::Stats& s) {
    std::printf("%-10s %12zu %12.2f %10.1f %16.0f %12zu\n", name, s.ops, s.total_ns / 1e6,
                s.ns_per_op, s.ops_per_sec, s.trades);
}

std::string compiler_id() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 1000000;
    std::uint64_t seed = 42;
    if (argc > 1) n = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
    if (argc > 2) seed = std::strtoull(argv[2], nullptr, 10);

    std::printf("C++ Limit Order Book -- native benchmark\n");
    std::printf("compiler : %s\n", compiler_id().c_str());
    std::printf("n        : %zu orders per scenario\n", n);
    std::printf("seed     : %llu\n\n", static_cast<unsigned long long>(seed));

    std::printf("%-10s %12s %12s %10s %16s %12s\n", "scenario", "ops", "total(ms)", "ns/op",
                "ops/sec", "trades");

    print_row("add", lob::bench::bench_add(n, seed));
    print_row("cancel", lob::bench::bench_cancel(n, seed));
    print_row("match", lob::bench::bench_match(n, seed));
    print_row("mixed", lob::bench::bench_mixed(n, seed));

    const auto clock = lob::bench::bench_clock(n, seed);
    std::printf("\n%-10s %12zu %12.2f %10.1f %16.0f %12s\n", "clock", clock.ops,
                clock.total_ns / 1e6, clock.ns_per_op, clock.ops_per_sec, "-");

    return 0;
}
