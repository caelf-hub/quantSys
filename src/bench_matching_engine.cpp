#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <x86intrin.h>
#include <pthread.h>
#include <sched.h>

#include "book/matching_engine.hpp"

using namespace book;

struct SyntheticOrder {
    uint64_t ref;
    char side;
    uint32_t price_raw;
    uint32_t shares;
};

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

// Generates a realistic mix of resting and crossing orders around a
// moving mid-price, so the matching engine does real work (some orders
// rest, some sweep one or more price levels) rather than a degenerate
// all-resting or all-crossing workload.
static std::vector<SyntheticOrder> generate_orders(size_t count, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> shares_dist(1, 500);
    std::uniform_real_distribution<double> offset_dist(-0.50, 0.50);

    std::vector<SyntheticOrder> orders;
    orders.reserve(count);

    double mid = 150.00;
    for (size_t i = 0; i < count; ++i) {
        char side = side_dist(rng) == 0 ? 'B' : 'S';
        double offset = offset_dist(rng);
        double price = side == 'B' ? mid + offset : mid - offset; // biases some crossing
        if (price < 100.00) price = 100.00;
        if (price > 200.00) price = 200.00;

        orders.push_back(SyntheticOrder{
            /*ref=*/i + 1, side, px(price), static_cast<uint32_t>(shares_dist(rng))});

        mid += (rng() % 3 - 1) * 0.01; // small random walk
        if (mid < 120.00) mid = 120.00;
        if (mid > 180.00) mid = 180.00;
    }
    return orders;
}

static double cycles_per_ns() {
    using namespace std::chrono;
    unsigned aux;
    auto t0 = steady_clock::now();
    uint64_t c0 = __rdtscp(&aux);
    std::this_thread::sleep_for(milliseconds(200));
    auto t1 = steady_clock::now();
    uint64_t c1 = __rdtscp(&aux);
    double ns = static_cast<double>(duration_cast<nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns;
}

static void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

int main() {
    pin_to_core(0);

    double cyc_per_ns = cycles_per_ns();
    std::cout << "Calibration: " << cyc_per_ns << " cycles/ns\n\n";

    constexpr size_t kNumOrders = 2'000'000;
    auto orders = generate_orders(kNumOrders, /*seed=*/42);

    MatchingEngine engine(px(100.00), px(200.00), px(0.01), /*max_orders=*/5'000'000);

    std::vector<uint64_t> latencies(kNumOrders);
    std::vector<Fill> fills;
    fills.reserve(64); // avoid reallocation inside the timed region for typical sweeps

    uint64_t total_fills = 0;

    unsigned aux;
    for (size_t i = 0; i < kNumOrders; ++i) {
        fills.clear();
        const SyntheticOrder& o = orders[i];

        uint64_t t0 = __rdtscp(&aux);
        engine.submit_order(o.ref, o.side, o.price_raw, o.shares, fills);
        uint64_t t1 = __rdtscp(&aux);

        latencies[i] = t1 - t0;
        total_fills += fills.size();
    }

    std::sort(latencies.begin(), latencies.end());
    auto to_ns = [&](uint64_t cyc) { return cyc / cyc_per_ns; };

    std::cout << "=== MatchingEngine::submit_order latency (" << kNumOrders << " orders) ===\n";
    std::cout << "Total fills generated: " << total_fills << "\n";
    std::cout << "Avg fills per order: "
              << (static_cast<double>(total_fills) / kNumOrders) << "\n";
    std::cout << "Final resting orders: " << engine.resting_order_count() << "\n\n";

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "p50:   " << to_ns(latencies[kNumOrders * 50 / 100]) << " ns\n";
    std::cout << "p90:   " << to_ns(latencies[kNumOrders * 90 / 100]) << " ns\n";
    std::cout << "p99:   " << to_ns(latencies[kNumOrders * 99 / 100]) << " ns\n";
    std::cout << "p99.9: " << to_ns(latencies[kNumOrders * 999 / 1000]) << " ns\n";
    std::cout << "max:   " << to_ns(latencies.back()) << " ns\n";

    return 0;
}
