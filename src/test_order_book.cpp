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

    // --- Best bid falls back across a gap, not just the adjacent tick ---
    OrderBook gapped(px(100.00), px(200.00), px(0.01), 10);
    assert(gapped.add_order(1, 'B', px(150.00), 10));
    assert(gapped.add_order(2, 'B', px(110.00), 10));
    assert(gapped.delete_order(1));
    assert(approx_eq(gapped.best_bid(), 110.00));
    assert(gapped.add_order(3, 'S', px(151.00), 10));
    assert(gapped.add_order(4, 'S', px(190.00), 10));
    assert(gapped.delete_order(3));
    assert(approx_eq(gapped.best_ask(), 190.00));
    std::cout << "[PASS] best price jumps a gap after the inside level empties\n";

    // --- Top-of-book size is only the best level, and it tracks cancels ---
    OrderBook sized(px(100.00), px(200.00), px(0.01), 10);
    assert(sized.add_order(1, 'B', px(150.00), 500));
    assert(sized.add_order(2, 'B', px(149.00), 800));
    assert(sized.best_bid_shares() == 500);
    assert(sized.add_order(3, 'S', px(151.00), 40));
    assert(sized.add_order(4, 'S', px(152.00), 90));
    assert(sized.best_ask_shares() == 40);
    assert(sized.cancel_shares(1, 100));
    assert(sized.best_bid_shares() == 400);
    assert(sized.delete_order(1));
    assert(sized.best_bid_shares() == 800);
    std::cout << "[PASS] best bid/ask share totals follow the best level\n";

    // Next occupied level is strictly behind the touch. A consistent book
    // has no live tick inside the reported best.
    OrderBook behind(px(100.00), px(200.00), px(0.01), 10);
    assert(behind.add_order(1, 'B', px(150.00), 10));
    assert(behind.add_order(2, 'B', px(149.99), 10));
    assert(behind.add_order(3, 'S', px(150.02), 10));
    assert(behind.add_order(4, 'S', px(150.05), 10));
    assert(approx_eq(behind.next_bid(), 149.99));
    assert(approx_eq(behind.next_ask(), 150.05));
    assert(behind.next_bid_gap_ticks() == 1);
    assert(behind.next_ask_gap_ticks() == 3);
    assert(!behind.touch_has_inside_level());
    std::cout << "[PASS] next occupied bid and ask sit behind the touch\n";

    // --- Each false return names why the event was dropped ---
    OrderBook band(px(100.00), px(200.00), px(0.01), 10);
    assert(!band.add_order(1, 'B', px(50.00), 10));
    assert(band.drop_counts().price_out_of_band == 1);
    assert(band.drop_counts().pool_exhausted == 0);
    std::cout << "[PASS] out-of-band add increments price_out_of_band\n";

    OrderBook pool(px(100.00), px(200.00), px(0.01), 1);
    assert(pool.add_order(1, 'B', px(150.00), 10));
    assert(!pool.add_order(2, 'B', px(151.00), 10));
    assert(pool.drop_counts().pool_exhausted == 1);
    assert(pool.drop_counts().price_out_of_band == 0);
    std::cout << "[PASS] second add on a full pool increments pool_exhausted\n";

    OrderBook unknown(px(100.00), px(200.00), px(0.01), 10);
    assert(!unknown.delete_order(1));
    assert(!unknown.cancel_shares(1, 1));
    assert(!unknown.execute_shares(1, 1));
    assert(!unknown.replace_order(1, 2, px(150.00), 1));
    assert(unknown.drop_counts().unknown_order == 4);
    std::cout << "[PASS] unknown cancel, execute, delete, and replace are counted\n";

    OrderBook duplicate(px(100.00), px(200.00), px(0.01), 10);
    assert(duplicate.add_order(1, 'B', px(150.00), 500));
    assert(!duplicate.add_order(1, 'S', px(149.00), 10));
    assert(duplicate.drop_counts().duplicate_order == 1);
    assert(duplicate.resting_order_count() == 1);
    assert(approx_eq(duplicate.best_bid(), 150.00));
    assert(duplicate.best_bid_shares() == 500);
    assert(duplicate.delete_order(1));
    assert(duplicate.resting_order_count() == 0);
    std::cout << "[PASS] duplicate order ref is rejected and the first order stays\n";

    OrderBook depth(px(100.00), px(200.00), px(0.01), 16);
    assert(depth.bid_depth_within(5) == 0 && depth.ask_depth_within(5) == 0);
    assert(depth.best_bid_raw() == 0 && depth.best_ask_raw() == 0);
    assert(depth.add_order(1, 'B', px(150.00), 100));
    assert(depth.add_order(2, 'B', px(150.00), 50));
    assert(depth.add_order(3, 'B', px(149.96), 20));  // 4 ticks behind: inside 5
    assert(depth.add_order(4, 'B', px(149.95), 7));   // 5 ticks behind: outside
    assert(depth.add_order(5, 'S', px(150.02), 30));
    assert(depth.add_order(6, 'S', px(150.06), 40));
    assert(depth.add_order(7, 'S', px(150.07), 9));
    assert(depth.bid_depth_within(5) == 170);
    assert(depth.bid_depth_within(1) == 150);
    assert(depth.ask_depth_within(5) == 70);
    assert(depth.best_bid_raw() == px(150.00) && depth.best_ask_raw() == px(150.02));
    std::cout << "[PASS] depth within N ticks counts the touch and levels inside the window\n";

    std::cout << "All OrderBook tests passed.\n";
    return 0;
}
