#pragma once

#include <memory>
#include <vector>

#include "order_book.hpp"

namespace book {

// Same purpose as MultiSymbolBook, but routes by ITCH stock_locate
// (a small, dense uint16_t assigned by the exchange) directly into a
// flat array instead of hashing a ticker string. Avoids string
// construction and hashing entirely in the hot path -- see
// bench_lookup_cost.cpp for the measured cost of the string-keyed
// approach this replaces (roughly 3x slower lookup alone).
class LocateIndexedBook {
public:
    LocateIndexedBook(uint32_t tick_size_raw, double band_pct, size_t max_orders_per_symbol,
                       size_t max_locate = 65536)
        : tick_size_(tick_size_raw),
          band_pct_(band_pct),
          max_orders_per_symbol_(max_orders_per_symbol),
          books_(max_locate) {}

    bool add_order(uint16_t locate, uint64_t order_ref, char side,
                    uint32_t price_raw, uint32_t shares) {
        return book_for(locate, price_raw).add_order(order_ref, side, price_raw, shares);
    }

    bool delete_order(uint16_t locate, uint64_t order_ref) {
        if (!books_[locate]) return false;
        return books_[locate]->delete_order(order_ref);
    }

    bool cancel_shares(uint16_t locate, uint64_t order_ref, uint32_t cancelled) {
        if (!books_[locate]) return false;
        return books_[locate]->cancel_shares(order_ref, cancelled);
    }

    bool execute_shares(uint16_t locate, uint64_t order_ref, uint32_t executed) {
        if (!books_[locate]) return false;
        return books_[locate]->execute_shares(order_ref, executed);
    }

    bool replace_order(uint16_t locate, uint64_t old_ref, uint64_t new_ref,
                        uint32_t price_raw, uint32_t shares) {
        if (!books_[locate]) return false;
        return books_[locate]->replace_order(old_ref, new_ref, price_raw, shares);
    }

    double best_bid(uint16_t locate) const {
        return books_[locate] ? books_[locate]->best_bid() : -1.0;
    }

    double best_ask(uint16_t locate) const {
        return books_[locate] ? books_[locate]->best_ask() : -1.0;
    }

    size_t resting_order_count(uint16_t locate) const {
        return books_[locate] ? books_[locate]->resting_order_count() : 0;
    }

    bool has_locate(uint16_t locate) const { return static_cast<bool>(books_[locate]); }

private:
    uint32_t tick_size_;
    double band_pct_;
    size_t max_orders_per_symbol_;
    std::vector<std::unique_ptr<OrderBook>> books_;

    OrderBook& book_for(uint16_t locate, uint32_t first_price_raw) {
        if (books_[locate]) return *books_[locate];

        uint32_t half = static_cast<uint32_t>(static_cast<double>(first_price_raw) * band_pct_);

        // Same stub-quote defense as MultiSymbolBook -- see Phase 5 README
        // notes for why this cap is necessary, not optional.
        constexpr uint32_t kMaxHalfWidthRaw = 1000 * 10000;
        if (half > kMaxHalfWidthRaw) half = kMaxHalfWidthRaw;

        uint32_t band_min = (first_price_raw > half) ? (first_price_raw - half) : tick_size_;
        uint32_t band_max = first_price_raw + half;
        if (band_max < first_price_raw) band_max = first_price_raw + kMaxHalfWidthRaw;

        books_[locate] = std::make_unique<OrderBook>(band_min, band_max, tick_size_,
                                                        max_orders_per_symbol_);
        return *books_[locate];
    }
};

} // namespace book
