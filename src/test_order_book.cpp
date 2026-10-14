#include <cassert>
#include <cmath>
#include <iostream>

#include "book/order_book.hpp"

using namespace book;

static bool approx_eq(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

// Price helper: dollars -> ITCH-style fixed point raw (4 implied decimals).
static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

int main() {
    // Ladder: $100.00 to $200.00, 1-cent ticks, room for 100 resting orders.
    OrderBook book(px(100.00), px(200.00), px(0.01), 100);

    // --- Add orders, verify best bid/ask tracking ---
    assert(book.add_order(/*ref=*/1, 'B', px(150.00), 500));
    assert(approx_eq(book.best_bid(), 150.00));
    std::cout << "[PASS] first bid becomes best bid\n";

    assert(book.add_order(/*ref=*/2, 'B', px(149.50), 300));
    assert(approx_eq(book.best_bid(), 150.00)); // higher bid still wins
    std::cout << "[PASS] best bid unaffected by lower bid\n";

    assert(book.add_order(/*ref=*/3, 'B', px(151.00), 200));
    assert(approx_eq(book.best_bid(), 151.00)); // new higher bid takes over
    std::cout << "[PASS] best bid updates to new higher bid\n";

    assert(book.add_order(/*ref=*/4, 'S', px(152.00), 400));
    assert(approx_eq(book.best_ask(), 152.00));
    std::cout << "[PASS] first ask becomes best ask\n";

    assert(book.add_order(/*ref=*/5, 'S', px(153.00), 100));
    assert(approx_eq(book.best_ask(), 152.00)); // higher ask doesn't win
    std::cout << "[PASS] best ask unaffected by higher ask\n";

    assert(book.add_order(/*ref=*/6, 'S', px(151.50), 250));
    assert(approx_eq(book.best_ask(), 151.50)); // new lower ask takes over
    std::cout << "[PASS] best ask updates to new lower ask\n";

    assert(book.resting_order_count() == 6);
    std::cout << "[PASS] resting order count correct after 6 adds\n";

    // --- Partial cancel ---
    assert(book.cancel_shares(/*ref=*/3, /*cancelled=*/50));
    assert(approx_eq(book.best_bid(), 151.00)); // still resting, just fewer shares
    std::cout << "[PASS] partial cancel leaves order resting at same price\n";

    // --- Full delete of the best bid; best should fall back to next level ---
    assert(book.delete_order(/*ref=*/3));
    assert(approx_eq(book.best_bid(), 150.00)); // falls back to order 1's price
    assert(book.resting_order_count() == 5);
    std::cout << "[PASS] deleting best bid correctly recomputes next-best\n";

    // --- Execute (partial fill) reduces shares, order stays resting ---
    assert(book.execute_shares(/*ref=*/1, /*executed=*/500)); // fully fills order 1
    assert(approx_eq(book.best_bid(), 149.50)); // order 1 now gone, falls to order 2
    assert(book.resting_order_count() == 4);
    std::cout << "[PASS] full execution removes order and recomputes best bid\n";

    // --- Replace: cancel-replace order 6 (best ask) to a worse price ---
    assert(book.replace_order(/*old_ref=*/6, /*new_ref=*/60, px(154.00), 250));
    assert(approx_eq(book.best_ask(), 152.00)); // falls back to order 4
    assert(book.resting_order_count() == 4); // 2,4,5,60 remain (3,1,6 gone, 60 added)
    std::cout << "[PASS] replace removes old order, adds new one, recomputes best ask\n";

    // --- Deleting a non-existent order fails cleanly ---
    assert(!book.delete_order(/*ref=*/9999));
    std::cout << "[PASS] deleting unknown order ref returns false\n";

    std::cout << "All OrderBook tests passed.\n";
    return 0;
}
