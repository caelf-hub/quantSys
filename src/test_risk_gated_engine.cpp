#include <cassert>
#include <iostream>

#include "book/risk_gated_engine.hpp"

using namespace book;

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

int main() {
    // --- Test 1: approved order reaches the book normally ---
    {
        RiskConfig cfg;
        RiskGatedEngine eng(px(1.00), px(2000.00), px(0.01), 1000, cfg);

        auto result = eng.submit_order(1, 'S', px(150.00), 100);
        assert(result.risk_result.approved);
        assert(result.matched_or_rested);
        assert(eng.resting_order_count() == 1);
        std::cout << "[PASS] approved order reaches and rests in the book\n";
    }

    // --- Test 2: risk-rejected order never touches the book ---
    {
        RiskConfig cfg;
        cfg.max_order_shares = 50;
        RiskGatedEngine eng(px(1.00), px(2000.00), px(0.01), 1000, cfg);

        auto result = eng.submit_order(1, 'B', px(150.00), 100); // exceeds max_order_shares
        assert(!result.risk_result.approved);
        assert(result.risk_result.reason == RiskRejectReason::OrderSizeExceedsMax);
        assert(!result.matched_or_rested);
        assert(eng.resting_order_count() == 0); // never reached the book
        std::cout << "[PASS] risk-rejected order never reaches the book\n";
    }

    // --- Test 3: price collar uses live best price as reference ---
    {
        RiskConfig cfg;
        cfg.price_collar_pct = 0.10;
        RiskGatedEngine eng(px(1.00), px(2000.00), px(0.01), 1000, cfg);

        // Establish a resting ask at $45.61 (mirrors the real Phase 3a data).
        eng.submit_order(1, 'S', px(45.61), 100);

        // A stub-quote-style sell far from the market should be rejected
        // against the current best ask as reference.
        auto bad = eng.submit_order(2, 'S', px(199999.99), 100);
        assert(!bad.risk_result.approved);
        assert(bad.risk_result.reason == RiskRejectReason::PriceOutsideCollar);
        std::cout << "[PASS] collar correctly uses live best price as reference\n";

        // A reasonable crossing buy should still work normally.
        auto good = eng.submit_order(3, 'B', px(45.61), 50);
        assert(good.risk_result.approved);
        assert(good.fills.size() == 1);
        std::cout << "[PASS] reasonable crossing order still matches normally\n";
    }

    // --- Test 4: position tracking only updates on approved orders ---
    {
        RiskConfig cfg;
        cfg.max_long_position = 100;
        RiskGatedEngine eng(px(1.00), px(2000.00), px(0.01), 1000, cfg);

        auto r1 = eng.submit_order(1, 'B', px(150.00), 100);
        assert(r1.risk_result.approved);
        assert(eng.risk().net_position() == 100);

        // This would breach the limit; must be rejected and must NOT
        // change net_position.
        auto r2 = eng.submit_order(2, 'B', px(150.00), 50);
        assert(!r2.risk_result.approved);
        assert(eng.risk().net_position() == 100); // unchanged
        std::cout << "[PASS] position only updates on approved orders, not rejects\n";
    }

    // --- Test 5: kill switch blocks all submissions through the gate ---
    {
        RiskConfig cfg;
        RiskGatedEngine eng(px(1.00), px(2000.00), px(0.01), 1000, cfg);
        eng.risk().trip_kill_switch();

        auto result = eng.submit_order(1, 'B', px(150.00), 10);
        assert(!result.risk_result.approved);
        assert(result.risk_result.reason == RiskRejectReason::KillSwitchActive);
        assert(eng.resting_order_count() == 0);
        std::cout << "[PASS] kill switch blocks submissions through the gated engine\n";
    }

    std::cout << "All RiskGatedEngine integration tests passed.\n";
    return 0;
}
