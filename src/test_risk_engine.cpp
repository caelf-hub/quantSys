#include <cassert>
#include <iostream>

#include "book/risk_engine.hpp"

using namespace book;

static uint32_t px(double dollars) {
    return static_cast<uint32_t>(dollars * 10000.0 + 0.5);
}

int main() {
    // --- Test 1: order within all limits is approved ---
    {
        RiskConfig cfg;
        RiskEngine risk(cfg);
        auto result = risk.check_order('B', px(150.00), 100, px(150.00));
        assert(result.approved);
        std::cout << "[PASS] order within all limits approved\n";
    }

    // --- Test 2: order size exceeds max ---
    {
        RiskConfig cfg;
        cfg.max_order_shares = 500;
        RiskEngine risk(cfg);
        auto result = risk.check_order('B', px(150.00), 600, px(150.00));
        assert(!result.approved);
        assert(result.reason == RiskRejectReason::OrderSizeExceedsMax);
        std::cout << "[PASS] oversized order rejected\n";
    }

    // --- Test 3: order value exceeds max (fat-finger price catch) ---
    {
        RiskConfig cfg;
        cfg.max_order_value_raw = static_cast<uint64_t>(px(10000.00)) * 100; // $1M cap
        RiskEngine risk(cfg);
        // 100 shares at $150,000 = way over any sane cap, simulates a
        // fat-finger extra zero on the price.
        auto result = risk.check_order('B', px(150000.00), 100, px(150000.00));
        assert(!result.approved);
        assert(result.reason == RiskRejectReason::OrderValueExceedsMax);
        std::cout << "[PASS] excessive order value rejected (fat-finger catch)\n";
    }

    // --- Test 4: price collar rejects orders far from reference ---
    {
        RiskConfig cfg;
        cfg.price_collar_pct = 0.10; // 10%
        RiskEngine risk(cfg);
        // Reference (best ask) is $45.61; this mirrors the real stub
        // quote we found in Phase 3a real data ($199,999.99).
        auto result = risk.check_order('S', px(199999.99), 100, px(45.61));
        assert(!result.approved);
        assert(result.reason == RiskRejectReason::PriceOutsideCollar);
        std::cout << "[PASS] stub-quote-style price correctly rejected by collar\n";
    }

    // --- Test 5: price collar allows reasonable deviation ---
    {
        RiskConfig cfg;
        cfg.price_collar_pct = 0.10;
        RiskEngine risk(cfg);
        // 5% away from reference, within the 10% collar.
        auto result = risk.check_order('B', px(147.00), 100, px(140.00));
        assert(result.approved);
        std::cout << "[PASS] order within collar tolerance approved\n";
    }

    // --- Test 6: no reference price skips collar check (first order case) ---
    {
        RiskConfig cfg;
        RiskEngine risk(cfg);
        auto result = risk.check_order('B', px(999.00), 100, /*reference=*/0);
        assert(result.approved); // collar skipped, nothing else should block it
        std::cout << "[PASS] missing reference price correctly skips collar check\n";
    }

    // --- Test 7: position limit breach ---
    {
        RiskConfig cfg;
        cfg.max_long_position = 1000;
        RiskEngine risk(cfg);
        risk.record_fill_or_rest('B', 900); // net position now +900
        assert(risk.net_position() == 900);

        auto result = risk.check_order('B', px(150.00), 200, px(150.00)); // would push to 1100
        assert(!result.approved);
        assert(result.reason == RiskRejectReason::PositionLimitBreach);
        std::cout << "[PASS] position limit breach correctly rejected\n";

        auto ok_result = risk.check_order('B', px(150.00), 100, px(150.00)); // exactly 1000
        assert(ok_result.approved);
        std::cout << "[PASS] order exactly at position limit boundary approved\n";
    }

    // --- Test 8: short position limit ---
    {
        RiskConfig cfg;
        cfg.max_short_position = -500;
        RiskEngine risk(cfg);
        risk.record_fill_or_rest('S', 400); // net position now -400
        auto result = risk.check_order('S', px(150.00), 200, px(150.00)); // would push to -600
        assert(!result.approved);
        assert(result.reason == RiskRejectReason::PositionLimitBreach);
        std::cout << "[PASS] short position limit correctly rejected\n";
    }

    // --- Test 9: kill switch blocks everything regardless of other limits ---
    {
        RiskConfig cfg;
        RiskEngine risk(cfg);
        risk.trip_kill_switch();
        auto result = risk.check_order('B', px(1.00), 1, px(1.00)); // trivially fine order
        assert(!result.approved);
        assert(result.reason == RiskRejectReason::KillSwitchActive);
        std::cout << "[PASS] kill switch rejects even a trivially valid order\n";

        risk.reset_kill_switch();
        auto result2 = risk.check_order('B', px(1.00), 1, px(1.00));
        assert(result2.approved);
        std::cout << "[PASS] resetting kill switch restores normal operation\n";
    }

    std::cout << "All RiskEngine tests passed.\n";
    return 0;
}
