#include <cassert>
#include <iostream>

#include "book/symbol_router.hpp"

using namespace book;

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

int main() {
    RiskConfig cfg;
    SymbolRouter router(px(1.00), px(2000.00), px(0.01), 1000, cfg);

    // --- Test 1: symbols are created lazily, start isolated ---
    assert(router.symbol_count() == 0);
    assert(!router.has_symbol("AAPL"));

    auto r1 = router.submit_order("AAPL", 1, 'S', px(150.00), 100);
    assert(r1.risk_result.approved);
    assert(router.symbol_count() == 1);
    assert(router.has_symbol("AAPL"));
    assert(!router.has_symbol("MSFT")); // untouched
    std::cout << "[PASS] symbols created lazily, unrelated symbols unaffected\n";

    // --- Test 2: two symbols maintain fully independent books ---
    router.submit_order("MSFT", 2, 'S', px(300.00), 50);
    assert(router.symbol_count() == 2);
    assert(router.best_ask("AAPL") == 150.00);
    assert(router.best_ask("MSFT") == 300.00);
    std::cout << "[PASS] two symbols maintain independent best-ask prices\n";

    // A crossing buy on AAPL must not fill or affect MSFT's resting order.
    auto fill_result = router.submit_order("AAPL", 3, 'B', px(150.00), 100);
    assert(fill_result.fills.size() == 1);
    assert(router.resting_order_count("AAPL") == 0);
    assert(router.resting_order_count("MSFT") == 1); // untouched
    std::cout << "[PASS] fill on one symbol does not affect another symbol's book\n";

    // --- Test 3: risk state (position) is independent per symbol ---
    router.submit_order("AAPL", 4, 'B', px(150.00), 200); // AAPL flat, then long 200
    // MSFT is still short 50 from the resting sell. Buying 10 covers part of it.
    router.submit_order("MSFT", 5, 'B', px(300.00), 10);
    assert(router.risk_for("AAPL").net_position() == 200);
    assert(router.risk_for("MSFT").net_position() == -40);
    std::cout << "[PASS] net position tracked independently per symbol\n";

    // --- Test 4: kill switch on one symbol doesn't affect another ---
    router.risk_for("AAPL").trip_kill_switch();
    auto blocked = router.submit_order("AAPL", 6, 'B', px(150.00), 10);
    assert(!blocked.risk_result.approved);
    assert(blocked.risk_result.reason == RiskRejectReason::KillSwitchActive);

    auto still_ok = router.submit_order("MSFT", 7, 'B', px(300.00), 10);
    assert(still_ok.risk_result.approved); // MSFT unaffected by AAPL's kill switch
    std::cout << "[PASS] kill switch is per-symbol, does not affect other symbols\n";

    // --- Test 5: cancel on nonexistent symbol fails cleanly ---
    assert(!router.cancel_order("GOOG", 999));
    std::cout << "[PASS] cancel on nonexistent symbol fails cleanly, no crash\n";

    std::cout << "All SymbolRouter tests passed.\n";
    return 0;
}
