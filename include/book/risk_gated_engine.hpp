#pragma once

#include <vector>

#include "matching_engine.hpp"
#include "risk_engine.hpp"

namespace book {

struct OrderSubmitResult {
    bool matched_or_rested; // true if it reached the matching engine at all
    RiskCheckResult risk_result;
    std::vector<Fill> fills;
};

// Wraps MatchingEngine with a mandatory pre-trade risk check. Every
// order passes through RiskEngine::check_order() before reaching the
// book; rejected orders never touch matching logic or book state.
class RiskGatedEngine {
public:
    RiskGatedEngine(uint32_t min_price_raw, uint32_t max_price_raw, uint32_t tick_size_raw,
                     size_t max_orders, RiskConfig risk_config)
        : engine_(min_price_raw, max_price_raw, tick_size_raw, max_orders),
          risk_(risk_config) {}

    OrderSubmitResult submit_order(uint64_t order_ref, char side, uint32_t price_raw,
                                     uint32_t shares) {
        OrderSubmitResult result;
        uint32_t reference_price_raw = reference_price_for(side);

        result.risk_result = risk_.check_order(side, price_raw, shares, reference_price_raw);
        if (!result.risk_result.approved) {
            result.matched_or_rested = false;
            return result;
        }

        result.matched_or_rested = engine_.submit_order(order_ref, side, price_raw, shares,
                                                           result.fills);
        if (result.matched_or_rested) {
            risk_.record_fill_or_rest(side, shares);
        }
        return result;
    }

    bool cancel_order(uint64_t order_ref) { return engine_.cancel_order(order_ref); }

    double best_bid() const { return engine_.best_bid(); }
    double best_ask() const { return engine_.best_ask(); }
    size_t resting_order_count() const { return engine_.resting_order_count(); }

    RiskEngine& risk() { return risk_; }
    const RiskEngine& risk() const { return risk_; }

private:
    MatchingEngine engine_;
    RiskEngine risk_;

    // Uses the opposite side's best price as the collar reference --
    // a buy is checked against the best ask, a sell against the best
    // bid, matching how a real pre-trade check would reference the
    // prevailing market. Falls back to 0 (collar skipped) if no
    // opposite-side liquidity exists yet.
    uint32_t reference_price_for(char side) const {
        double ref = (side == 'B') ? engine_.best_ask() : engine_.best_bid();
        if (ref < 0) return 0;
        return static_cast<uint32_t>(ref * 10000.0 + 0.5);
    }
};

} // namespace book
