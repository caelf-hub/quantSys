#pragma once

#include <memory>
#include <utility>
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
          slots_(max_locate) {}

    // Per-locate pool sizes, e.g. from plan_capacity. Applies to books not yet
    // created. A locate past the end of the vector keeps max_orders_per_symbol.
    void set_capacities(std::vector<uint32_t> capacities) {
        capacities_ = std::move(capacities);
    }

    size_t pool_capacity(uint16_t locate) const {
        if (static_cast<size_t>(locate) < capacities_.size()) return capacities_[locate];
        return max_orders_per_symbol_;
    }

    bool add_order(uint16_t locate, uint64_t order_ref, char side,
                    uint32_t price_raw, uint32_t shares) {
        if (!in_range(locate)) {
            ++missing_unknown_;
            return false;
        }
        return note(locate, book_for(locate, price_raw).add_order(order_ref, side, price_raw, shares));
    }

    bool delete_order(uint16_t locate, uint64_t order_ref) {
        if (!in_range(locate) || !slots_[locate].book) {
            ++missing_unknown_;
            return false;
        }
        return note(locate, slots_[locate].book->delete_order(order_ref));
    }

    bool cancel_shares(uint16_t locate, uint64_t order_ref, uint32_t cancelled) {
        if (!in_range(locate) || !slots_[locate].book) {
            ++missing_unknown_;
            return false;
        }
        return note(locate, slots_[locate].book->cancel_shares(order_ref, cancelled));
    }

    bool execute_shares(uint16_t locate, uint64_t order_ref, uint32_t executed) {
        if (!in_range(locate) || !slots_[locate].book) {
            ++missing_unknown_;
            return false;
        }
        return note(locate, slots_[locate].book->execute_shares(order_ref, executed));
    }

    bool replace_order(uint16_t locate, uint64_t old_ref, uint64_t new_ref,
                        uint32_t price_raw, uint32_t shares) {
        if (!in_range(locate) || !slots_[locate].book) {
            ++missing_unknown_;
            return false;
        }
        return note(locate, slots_[locate].book->replace_order(old_ref, new_ref, price_raw, shares));
    }

    double best_bid(uint16_t locate) const {
        if (!in_range(locate) || !slots_[locate].book) return -1.0;
        return slots_[locate].book->best_bid();
    }

    double best_ask(uint16_t locate) const {
        if (!in_range(locate) || !slots_[locate].book) return -1.0;
        return slots_[locate].book->best_ask();
    }

    size_t resting_order_count(uint16_t locate) const {
        if (!in_range(locate) || !slots_[locate].book) return 0;
        return slots_[locate].book->resting_order_count();
    }

    size_t ladder_ticks(uint16_t locate) const {
        if (!in_range(locate) || !slots_[locate].book) return 0;
        return slots_[locate].book->ladder_ticks();
    }

    uint64_t applied_events(uint16_t locate) const {
        if (!in_range(locate)) return 0;
        return slots_[locate].applied;
    }

    bool has_locate(uint16_t locate) const {
        return in_range(locate) && static_cast<bool>(slots_[locate].book);
    }

    DropCounts locate_drops(uint16_t locate) const {
        if (!in_range(locate) || !slots_[locate].book) return {};
        return slots_[locate].book->drop_counts();
    }

    // Sum of per-book drops, plus cancels and executes for a locate that
    // never received an order. Those never reach an OrderBook.
    DropCounts drop_counts() const {
        DropCounts total;
        total.unknown_order = missing_unknown_;
        for (const auto& slot : slots_) {
            if (!slot.book) continue;
            const DropCounts& counts = slot.book->drop_counts();
            total.price_out_of_band += counts.price_out_of_band;
            total.pool_exhausted += counts.pool_exhausted;
            total.unknown_order += counts.unknown_order;
            total.duplicate_order += counts.duplicate_order;
        }
        return total;
    }

private:
    struct Slot {
        std::unique_ptr<OrderBook> book;
        uint64_t applied = 0;
    };

    uint32_t tick_size_;
    double band_pct_;
    size_t max_orders_per_symbol_;
    uint64_t missing_unknown_ = 0;
    std::vector<uint32_t> capacities_;
    std::vector<Slot> slots_;

    bool in_range(uint16_t locate) const {
        return static_cast<size_t>(locate) < slots_.size();
    }

    // Count on the same cache line as the book pointer already loaded.
    bool note(uint16_t locate, bool ok) {
        if (ok) ++slots_[locate].applied;
        return ok;
    }

    OrderBook& book_for(uint16_t locate, uint32_t first_price_raw) {
        if (slots_[locate].book) return *slots_[locate].book;

        uint32_t half = static_cast<uint32_t>(static_cast<double>(first_price_raw) * band_pct_);

        // Same stub-quote defense as MultiSymbolBook -- see Phase 5 README
        // notes for why this cap is necessary, not optional.
        constexpr uint32_t kMaxHalfWidthRaw = 1000 * 10000;
        if (half > kMaxHalfWidthRaw) half = kMaxHalfWidthRaw;

        uint32_t band_min = (first_price_raw > half) ? (first_price_raw - half) : tick_size_;
        uint32_t band_max = first_price_raw + half;
        if (band_max < first_price_raw) band_max = UINT32_MAX;
        if (band_max < band_min) band_max = band_min;

        slots_[locate].book = std::make_unique<OrderBook>(band_min, band_max, tick_size_,
                                                          pool_capacity(locate));
        return *slots_[locate].book;
    }
};

} // namespace book
