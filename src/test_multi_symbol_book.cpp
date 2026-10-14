#include <cassert>
#include <cmath>
#include <iostream>

#include "book/multi_symbol_book.hpp"

using namespace book;

static bool approx_eq(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

int main() {
    // tick=$0.01, band=+/-50% of first observed price per symbol.
    MultiSymbolBook market(px(0.01), 0.5, /*max_orders_per_symbol=*/1000);

    // --- Test 1: a cheap stock's band is sized around ITS price, not
    //     some global range ---
    assert(market.add_order("CHEAP", 1, 'S', px(6.00), 100)); // $6 stock, band ~$3-$9
    assert(approx_eq(market.best_ask("CHEAP"), 6.00));
    std::cout << "[PASS] cheap stock accepts order at its own price\n";

    // An order far outside a $3-$9 band for THIS symbol should be rejected.
    assert(!market.add_order("CHEAP", 2, 'S', px(50.00), 100));
    std::cout << "[PASS] cheap stock's dynamic band correctly rejects a wildly higher price\n";

    // --- Test 2: an expensive stock gets its own, much wider band ---
    assert(market.add_order("PRICEY", 3, 'S', px(300.00), 100)); // band ~$150-$450
    assert(approx_eq(market.best_ask("PRICEY"), 300.00));

    // This is the exact scenario that broke with a global $1-$200 range:
    // a price above $200 for an expensive stock must now be accepted,
    // since the band is centered on ITS OWN price, not a shared ceiling.
    assert(market.add_order("PRICEY", 4, 'B', px(250.00), 50));
    assert(approx_eq(market.best_bid("PRICEY"), 250.00));
    std::cout << "[PASS] expensive stock's own band accepts a price above a hypothetical "
                 "global $200 ceiling (the exact bug this fix addresses)\n";

    // --- Test 3: symbols remain fully isolated from each other ---
    assert(market.resting_order_count("CHEAP") == 1); // only the $6 order
    assert(market.resting_order_count("PRICEY") == 2);
    std::cout << "[PASS] symbols remain isolated under dynamic per-symbol bands\n";

    std::cout << "All MultiSymbolBook tests passed.\n";
    return 0;
}
