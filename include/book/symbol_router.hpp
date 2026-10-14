#pragma once

#include <string>
#include <unordered_map>

#include "risk_gated_engine.hpp"

namespace book {

// Routes orders to a per-symbol RiskGatedEngine, created lazily on first
// use. Each symbol's book and risk state are fully independent -- an
// order for one ticker can never affect another's book or position.
class SymbolRouter {
public:
    SymbolRouter(uint32_t default_min_price_raw, uint32_t default_max_price_raw,
                 uint32_t default_tick_size_raw, size_t default_max_orders_per_symbol,
                 RiskConfig default_risk_config)
        : default_min_(default_min_price_raw),
          default_max_(default_max_price_raw),
          default_tick_(default_tick_size_raw),
          default_max_orders_(default_max_orders_per_symbol),
          default_risk_config_(default_risk_config) {}

    OrderSubmitResult submit_order(const std::string& symbol, uint64_t order_ref, char side,
                                     uint32_t price_raw, uint32_t shares) {
        RiskGatedEngine& engine = engine_for(symbol);
        return engine.submit_order(order_ref, side, price_raw, shares);
    }

    bool cancel_order(const std::string& symbol, uint64_t order_ref) {
        auto it = engines_.find(symbol);
        if (it == engines_.end()) return false;
        return it->second.cancel_order(order_ref);
    }

    double best_bid(const std::string& symbol) const {
        auto it = engines_.find(symbol);
        return it == engines_.end() ? -1.0 : it->second.best_bid();
    }

    double best_ask(const std::string& symbol) const {
        auto it = engines_.find(symbol);
        return it == engines_.end() ? -1.0 : it->second.best_ask();
    }

    size_t resting_order_count(const std::string& symbol) const {
        auto it = engines_.find(symbol);
        return it == engines_.end() ? 0 : it->second.resting_order_count();
    }

    size_t symbol_count() const { return engines_.size(); }

    bool has_symbol(const std::string& symbol) const {
        return engines_.find(symbol) != engines_.end();
    }

    // Exposes a symbol's RiskEngine directly, e.g. to trip a per-symbol
    // kill switch or inspect net position. Creates the symbol lazily if
    // it doesn't exist yet.
    RiskEngine& risk_for(const std::string& symbol) {
        return engine_for(symbol).risk();
    }

private:
    uint32_t default_min_;
    uint32_t default_max_;
    uint32_t default_tick_;
    size_t default_max_orders_;
    RiskConfig default_risk_config_;

    std::unordered_map<std::string, RiskGatedEngine> engines_;

    RiskGatedEngine& engine_for(const std::string& symbol) {
        auto it = engines_.find(symbol);
        if (it != engines_.end()) return it->second;

        auto result = engines_.emplace(
            symbol, RiskGatedEngine(default_min_, default_max_, default_tick_,
                                     default_max_orders_, default_risk_config_));
        return result.first->second;
    }
};

} // namespace book
