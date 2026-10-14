#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace book {

enum class RiskRejectReason {
    None,
    KillSwitchActive,
    OrderSizeExceedsMax,
    OrderValueExceedsMax,
    PriceOutsideCollar,
    PositionLimitBreach,
};

inline const char* to_string(RiskRejectReason reason) {
    switch (reason) {
        case RiskRejectReason::None: return "None";
        case RiskRejectReason::KillSwitchActive: return "KillSwitchActive";
        case RiskRejectReason::OrderSizeExceedsMax: return "OrderSizeExceedsMax";
        case RiskRejectReason::OrderValueExceedsMax: return "OrderValueExceedsMax";
        case RiskRejectReason::PriceOutsideCollar: return "PriceOutsideCollar";
        case RiskRejectReason::PositionLimitBreach: return "PositionLimitBreach";
        default: return "Unknown";
    }
}

struct RiskCheckResult {
    bool approved;
    RiskRejectReason reason = RiskRejectReason::None;
};

struct RiskConfig {
    uint32_t max_order_shares = 10'000;
    uint64_t max_order_value_raw = 1'000'000ULL * 10000ULL; // $1,000,000, 4 implied decimals
    double price_collar_pct = 0.10; // reject if price is >10% away from reference
    int64_t max_long_position = 100'000;
    int64_t max_short_position = -100'000;
};

// Pre-trade risk gate. Every order should pass through check_order()
// before being submitted to the matching engine. Tracks net position
// per side (buys increase, sells decrease) to enforce position limits.
class RiskEngine {
public:
    explicit RiskEngine(RiskConfig config) : config_(config) {}

    // Checks a proposed order against all configured risk limits.
    // `reference_price_raw` is the current best bid/ask (or mid) used
    // for the price collar check; pass 0 if no reference is available
    // yet (collar check is skipped in that case -- can't collar against
    // nothing, e.g. the very first order for a symbol).
    RiskCheckResult check_order(char side, uint32_t price_raw, uint32_t shares,
                                 uint32_t reference_price_raw) const {
        if (kill_switch_active_) {
            return {false, RiskRejectReason::KillSwitchActive};
        }

        if (shares > config_.max_order_shares) {
            return {false, RiskRejectReason::OrderSizeExceedsMax};
        }

        uint64_t order_value_raw = static_cast<uint64_t>(price_raw) * static_cast<uint64_t>(shares);
        if (order_value_raw > config_.max_order_value_raw) {
            return {false, RiskRejectReason::OrderValueExceedsMax};
        }

        if (reference_price_raw > 0) {
            double ref = static_cast<double>(reference_price_raw);
            double px = static_cast<double>(price_raw);
            double pct_away = std::abs(px - ref) / ref;
            if (pct_away > config_.price_collar_pct) {
                return {false, RiskRejectReason::PriceOutsideCollar};
            }
        }

        int64_t signed_shares = (side == 'B') ? static_cast<int64_t>(shares)
                                               : -static_cast<int64_t>(shares);
        int64_t projected_position = net_position_ + signed_shares;
        if (projected_position > config_.max_long_position ||
            projected_position < config_.max_short_position) {
            return {false, RiskRejectReason::PositionLimitBreach};
        }

        return {true, RiskRejectReason::None};
    }

    // Call after an order is actually accepted/filled to update tracked
    // position. Only call this for the quantity that actually executed
    // or rested -- not for rejected orders.
    void record_fill_or_rest(char side, uint32_t shares) {
        int64_t signed_shares = (side == 'B') ? static_cast<int64_t>(shares)
                                               : -static_cast<int64_t>(shares);
        net_position_ += signed_shares;
    }

    void trip_kill_switch() { kill_switch_active_ = true; }
    void reset_kill_switch() { kill_switch_active_ = false; }
    bool kill_switch_active() const { return kill_switch_active_; }

    int64_t net_position() const { return net_position_; }

private:
    RiskConfig config_;
    int64_t net_position_ = 0;
    bool kill_switch_active_ = false;
};

} // namespace book
