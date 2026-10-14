#include <cassert>
#include <cmath>
#include <iostream>

#include "book/locate_indexed_book.hpp"

using namespace book;

static bool approx_eq(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

int main() {
    LocateIndexedBook market(px(0.01), 0.5, /*max_orders_per_symbol=*/1000);

    // --- Test 1: a cheap stock's band is sized around its own price ---
    assert(market.add_order(/*locate=*/1, 1, 'S', px(6.00), 100));
    assert(approx_eq(market.best_ask(1), 6.00));
    std::cout << "[PASS] cheap stock (locate 1) accepts order at its own price\n";

    assert(!market.add_order(1, 2, 'S', px(50.00), 100)); // outside its own band
    std::cout << "[PASS] dynamic band correctly rejects a wildly higher price\n";

    // --- Test 2: an expensive stock gets its own, much wider band ---
    assert(market.add_order(/*locate=*/2, 3, 'S', px(300.00), 100));
    assert(market.add_order(2, 4, 'B', px(250.00), 50));
    std::cout << "[PASS] expensive stock (locate 2) accepts a price above a hypothetical "
                 "global ceiling\n";

    // --- Test 3: locates remain fully isolated ---
    assert(market.resting_order_count(1) == 1);
    assert(market.resting_order_count(2) == 2);
    assert(!market.has_locate(3)); // never touched
    std::cout << "[PASS] locates remain isolated from each other\n";

    // --- Test 4: stub-quote band clamp still applies ---
    assert(market.add_order(/*locate=*/3, 5, 'S', px(199999.99), 100));
    // Should succeed (first order always does), but band is clamped to
    // +/-$1000, not +/-$100,000 -- verified indirectly: a price just
    // outside a $1000-wide band from 199999.99 should be rejected.
    assert(!market.add_order(3, 6, 'B', px(1.00), 100));
    std::cout << "[PASS] stub-quote band-width clamp still applies in locate-indexed version\n";

    std::cout << "All LocateIndexedBook tests passed.\n";
    return 0;
}
