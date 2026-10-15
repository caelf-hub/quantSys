#pragma once

#include <string>
#include <unordered_map>

#include "order_book.hpp"

namespace book {

// Routes real exchange event-stream messages (Add/Cancel/Delete/Replace/
// Executed) to a per-symbol OrderBook, created lazily. Unlike
// RiskGatedEngine/MatchingEngine, this performs no matching -- it only
// reconstructs book state as reported by the exchange.
//
// Each symbol's price ladder is sized dynamically around the price of
// its FIRST observed order (+/- band_pct), not one global range shared
// by every symbol. A single global range either wastes memory on cheap
// stocks or silently truncates expensive ones (observed directly: a
// global $1-$200 range silently dropped a real $200+ COST ask). Per-
// symbol dynamic sizing fixes both problems and uses less total memory.
class MultiSymbolBook {
public:
    MultiSymbolBook(uint32_t tick_size_raw, double band_pct, size_t max_orders_per_symbol)
        : tick_size_(tick_size_raw),
          band_pct_(band_pct),
          max_orders_per_symbol_(max_orders_per_symbol) {}

    bool add_order(const std::string& symbol, uint64_t order_ref, char side,
                    uint32_t price_raw, uint32_t shares) {
        return book_for(symbol, price_raw).add_order(order_ref, side, price_raw, shares);
    }

    bool delete_order(const std::string& symbol, uint64_t order_ref) {
        auto it = books_.find(symbol);
        if (it == books_.end()) return false;
        return it->second.delete_order(order_ref);
    }

    bool cancel_shares(const std::string& symbol, uint64_t order_ref, uint32_t cancelled) {
        auto it = books_.find(symbol);
        if (it == books_.end()) return false;
        return it->second.cancel_shares(order_ref, cancelled);
    }

    bool execute_shares(const std::string& symbol, uint64_t order_ref, uint32_t executed) {
        auto it = books_.find(symbol);
        if (it == books_.end()) return false;
        return it->second.execute_shares(order_ref, executed);
    }

    bool replace_order(const std::string& symbol, uint64_t old_ref, uint64_t new_ref,
                        uint32_t price_raw, uint32_t shares) {
        auto it = books_.find(symbol);
        if (it == books_.end()) return false;
        return it->second.replace_order(old_ref, new_ref, price_raw, shares);
    }

    double best_bid(const std::string& symbol) const {
        auto it = books_.find(symbol);
        return it == books_.end() ? -1.0 : it->second.best_bid();
    }

    double best_ask(const std::string& symbol) const {
        auto it = books_.find(symbol);
        return it == books_.end() ? -1.0 : it->second.best_ask();
    }

    size_t resting_order_count(const std::string& symbol) const {
        auto it = books_.find(symbol);
        return it == books_.end() ? 0 : it->second.resting_order_count();
    }

    size_t symbol_count() const { return books_.size(); }

    size_t ladder_ticks(const std::string& symbol) const {
        auto it = books_.find(symbol);
        return it == books_.end() ? 0 : it->second.ladder_ticks();
    }

    const std::unordered_map<std::string, OrderBook>& all_books() const { return books_; }

private:
    uint32_t tick_size_;
    double band_pct_;
    size_t max_orders_per_symbol_;

    std::unordered_map<std::string, OrderBook> books_;

    OrderBook& book_for(const std::string& symbol, uint32_t first_price_raw) {
        auto it = books_.find(symbol);
        if (it != books_.end()) return it->second;

        uint32_t half = static_cast<uint32_t>(static_cast<double>(first_price_raw) * band_pct_);

        // Absolute cap on band half-width, regardless of computed percentage.
        // Defends against stub quotes (e.g. $199,999.99, found for real in
        // Phase 3a data) blowing up a single symbol's memory footprint if
        // one happens to be that symbol's first observed order.
        constexpr uint32_t kMaxHalfWidthRaw = 1000 * 10000; // $1,000 max half-width
        if (half > kMaxHalfWidthRaw) half = kMaxHalfWidthRaw;

        uint32_t band_min = (first_price_raw > half) ? (first_price_raw - half) : tick_size_;
        uint32_t band_max = first_price_raw + half;
        if (band_max < first_price_raw) band_max = UINT32_MAX; // overflow guard
        if (band_max < band_min) band_max = band_min;

        auto result = books_.emplace(
            symbol, OrderBook(band_min, band_max, tick_size_, max_orders_per_symbol_));
        return result.first->second;
    }
};

} // namespace book
