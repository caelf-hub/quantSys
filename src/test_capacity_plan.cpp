#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "book/capacity_plan.hpp"
#include "book/locate_indexed_book.hpp"

using namespace book;

int main() {
    LiveOrderPeak peak(8);
    peak.on_add(1);
    peak.on_add(1);
    peak.on_delete(1);
    peak.on_add(1);
    peak.on_add(1);       // live 3, peak 3
    peak.on_delete(1);
    peak.on_delete(2);    // delete before any add stays at 0
    peak.on_add(2);
    peak.on_add(9);       // out of range, ignored
    assert(peak.peaks()[1] == 3);
    assert(peak.peaks()[2] == 1);
    assert(peak.peaks()[0] == 0);
    std::cout << "[PASS] live-order peak tracks adds minus deletes\n";

    const std::vector<uint32_t> peaks = {0, 10, 100, 1000};
    const CapacityPlan fits = plan_capacity(peaks, 2000);
    assert(fits.capacity == peaks);
    assert(fits.cut_locates.empty());
    assert(fits.total_slots == 1110);
    assert(fits.cap == UINT32_MAX);
    std::cout << "[PASS] a plan within budget grants every peak\n";

    const CapacityPlan cut = plan_capacity(peaks, 300);
    assert((cut.capacity == std::vector<uint32_t>{0, 10, 100, 190}));
    assert(cut.cap == 190);
    assert(cut.total_slots == 300);
    assert((cut.cut_locates == std::vector<uint16_t>{3}));
    std::cout << "[PASS] an over-budget plan water-fills and names the cut locate\n";

    const CapacityPlan none = plan_capacity(peaks, 0);
    assert(none.total_slots == 0);
    assert(none.cut_locates.size() == 3);
    std::cout << "[PASS] a zero budget cuts every active locate\n";

    // Sized locates do not exhaust; a cut locate still counts its drops.
    LocateIndexedBook market(/*tick=*/100, 0.5, /*max_orders_per_symbol=*/1, /*max_locate=*/8);
    market.set_capacities({0, 3, 10});
    assert(market.pool_capacity(1) == 3);
    assert(market.pool_capacity(5) == 1);
    for (uint64_t ref = 1; ref <= 3; ++ref) assert(market.add_order(1, ref, 'B', 1000000, 100));
    assert(!market.add_order(1, 4, 'B', 1000000, 100));
    for (uint64_t ref = 1; ref <= 10; ++ref) assert(market.add_order(2, ref, 'S', 1000000, 100));
    assert(market.add_order(5, 1, 'S', 1000000, 100));
    assert(!market.add_order(5, 2, 'S', 1000000, 100));
    assert(market.locate_drops(1).pool_exhausted == 1);
    assert(market.locate_drops(2).pool_exhausted == 0);
    assert(market.drop_counts().pool_exhausted == 2);
    std::cout << "[PASS] per-locate capacities size each book and drops stay counted\n";

    std::cout << "All capacity plan tests passed.\n";
    return 0;
}
