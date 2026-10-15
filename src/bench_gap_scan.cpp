#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <x86intrin.h>

#include "book/matching_engine.hpp"

using namespace book;

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

static uint64_t cycles() {
    unsigned aux;
    return __rdtscp(&aux);
}

// Times submit_order on a tight book and on a book whose prices sit on
// a 10-cent grid. The grid case is what made the empty-level scan show
// up at p99. This is not part of ctest.
static void run(const char* name, bool gapped) {
    constexpr size_t n = 400'000;
    MatchingEngine engine(px(100.00), px(200.00), px(0.01), n);
    std::vector<Fill> fills;
    fills.reserve(8);
    std::mt19937 rng(1);
    std::vector<uint64_t> lat(n);
    double mid = 150.0;
    for (size_t i = 0; i < n; ++i) {
        const char side = (rng() & 1) ? 'B' : 'S';
        double price = gapped ? (100.0 + static_cast<double>(rng() % 1000) * 0.10)
                               : (side == 'B' ? mid + 0.01 : mid - 0.01);
        if (price < 100.0) price = 100.0;
        if (price > 200.0) price = 200.0;
        fills.clear();
        const uint64_t t0 = cycles();
        engine.submit_order(i + 1, side, px(price), 100, fills);
        lat[i] = cycles() - t0;
        if (!gapped) {
            mid += static_cast<double>(static_cast<int>(rng() % 3) - 1) * 0.01;
            if (mid < 140.0) mid = 140.0;
            if (mid > 160.0) mid = 160.0;
        }
    }
    std::sort(lat.begin(), lat.end());
    unsigned aux;
    const auto wall0 = std::chrono::steady_clock::now();
    const uint64_t c0 = __rdtscp(&aux);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t c1 = __rdtscp(&aux);
    const double ns = std::chrono::duration<double, std::nano>(
                          std::chrono::steady_clock::now() - wall0)
                          .count();
    const double cyc_per_ns = static_cast<double>(c1 - c0) / ns;
    std::cout << name << "  p50=" << lat[n * 50 / 100] / cyc_per_ns
              << " ns  p99=" << lat[n * 99 / 100] / cyc_per_ns << " ns\n";
}

int main() {
    run("dense touch", false);
    run("gapped 10c", true);
    return 0;
}
