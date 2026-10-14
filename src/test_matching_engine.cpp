#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "book/matching_engine.hpp"

using namespace book;

static bool approx_eq(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

int main() {
    // --- Test 1: resting sell fully consumed by a crossing buy ---
    {
        MatchingEngine eng(px(100.00), px(200.00), px(0.01), 100);
        std::vector<Fill> fills;

        assert(eng.submit_order(/*ref=*/1, 'S', px(150.00), 100, fills));
        assert(fills.empty()); // no crossing liquidity yet, rests as ask
        assert(approx_eq(eng.best_ask(), 150.00));

        assert(eng.submit_order(/*ref=*/2, 'B', px(150.00), 100, fills));
        assert(fills.size() == 1);
        assert(fills[0].taker_ref == 2 && fills[0].maker_ref == 1);
        assert(fills[0].shares == 100);
        assert(fills[0].price_raw == px(150.00)); // executes at maker's price
        assert(eng.best_ask() == -1.0); // fully consumed, no asks left
        assert(eng.resting_order_count() == 0);
        std::cout << "[PASS] crossing buy fully consumes resting sell\n";
    }

    // --- Test 2: FIFO time priority at the same price level ---
    {
        MatchingEngine eng(px(100.00), px(200.00), px(0.01), 100);
        std::vector<Fill> fills;

        eng.submit_order(/*ref=*/1, 'S', px(150.00), 50, fills); // first in line
        eng.submit_order(/*ref=*/2, 'S', px(150.00), 50, fills); // second in line
        fills.clear();

        // Incoming buy for 60 shares should fill ref=1 completely (50)
        // then ref=2 partially (10), preserving time priority.
        eng.submit_order(/*ref=*/3, 'B', px(150.00), 60, fills);
        assert(fills.size() == 2);
        assert(fills[0].maker_ref == 1 && fills[0].shares == 50);
        assert(fills[1].maker_ref == 2 && fills[1].shares == 10);
        assert(eng.resting_order_count() == 1); // ref=2 still resting with 40 left
        std::cout << "[PASS] FIFO time priority respected within price level\n";
    }

    // --- Test 3: partial fill, remainder rests in book ---
    {
        MatchingEngine eng(px(100.00), px(200.00), px(0.01), 100);
        std::vector<Fill> fills;

        eng.submit_order(/*ref=*/1, 'S', px(150.00), 30, fills);
        fills.clear();

        // Incoming buy for 100 shares only finds 30 available; 70 should rest.
        eng.submit_order(/*ref=*/2, 'B', px(150.00), 100, fills);
        assert(fills.size() == 1 && fills[0].shares == 30);
        assert(eng.best_bid() == 150.00); // remainder of order 2 now rests as a bid
        assert(eng.resting_order_count() == 1);
        std::cout << "[PASS] partial fill leaves correct remainder resting\n";
    }

    // --- Test 4: multi-level sweep across several price levels ---
    {
        MatchingEngine eng(px(100.00), px(200.00), px(0.01), 100);
        std::vector<Fill> fills;

        eng.submit_order(/*ref=*/1, 'S', px(150.00), 10, fills);
        eng.submit_order(/*ref=*/2, 'S', px(150.50), 10, fills);
        eng.submit_order(/*ref=*/3, 'S', px(151.00), 10, fills);
        fills.clear();

        // Aggressive buy at 151.00 for 25 shares should sweep all three
        // levels: 10 @ 150.00, 10 @ 150.50, 5 @ 151.00.
        eng.submit_order(/*ref=*/4, 'B', px(151.00), 25, fills);
        assert(fills.size() == 3);
        assert(fills[0].price_raw == px(150.00) && fills[0].shares == 10);
        assert(fills[1].price_raw == px(150.50) && fills[1].shares == 10);
        assert(fills[2].price_raw == px(151.00) && fills[2].shares == 5);
        assert(approx_eq(eng.best_ask(), 151.00)); // 5 shares remain at this level
        std::cout << "[PASS] multi-level sweep fills best prices first\n";
    }

    // --- Test 5: non-crossing order simply rests, no fills ---
    {
        MatchingEngine eng(px(100.00), px(200.00), px(0.01), 100);
        std::vector<Fill> fills;

        eng.submit_order(/*ref=*/1, 'S', px(150.00), 100, fills);
        eng.submit_order(/*ref=*/2, 'B', px(149.00), 100, fills); // doesn't cross
        assert(fills.empty());
        assert(approx_eq(eng.best_bid(), 149.00));
        assert(approx_eq(eng.best_ask(), 150.00));
        std::cout << "[PASS] non-crossing order rests without generating fills\n";
    }

    // --- Test 6: cancel removes a resting order cleanly ---
    {
        MatchingEngine eng(px(100.00), px(200.00), px(0.01), 100);
        std::vector<Fill> fills;

        eng.submit_order(/*ref=*/1, 'B', px(149.00), 100, fills);
        assert(eng.cancel_order(1));
        assert(eng.best_bid() == -1.0);
        assert(!eng.cancel_order(1)); // already gone
        std::cout << "[PASS] cancel removes resting order and recomputes best price\n";
    }

    std::cout << "All MatchingEngine tests passed.\n";
    return 0;
}
