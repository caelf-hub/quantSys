#pragma once

#include <cstdint>
#include <vector>

#include "order_pool.hpp"

namespace book {

// One price level: a FIFO queue of orders (oldest first = best time
// priority) plus a running total of shares for fast top-of-book reads
// without walking the list.
struct PriceLevel {
    int32_t head = kNullIndex;
    int32_t tail = kNullIndex;
    uint64_t total_shares = 0;
    uint32_t order_count = 0;
};

// Flat array price ladder indexed directly by tick number. Ticks are
// price_raw / tick_size_raw -- O(1) lookup, no tree traversal. Bounded
// to a configurable price range per symbol to keep memory reasonable.
class PriceLadder {
public:
    PriceLadder(uint32_t min_price_raw, uint32_t max_price_raw, uint32_t tick_size_raw)
        : min_price_raw_(min_price_raw),
          tick_size_raw_(tick_size_raw),
          levels_((max_price_raw - min_price_raw) / tick_size_raw + 1) {}

    int32_t tick_index(uint32_t price_raw) const {
        if (price_raw < min_price_raw_) return -1;
        uint32_t idx = (price_raw - min_price_raw_) / tick_size_raw_;
        if (idx >= levels_.size()) return -1;
        return static_cast<int32_t>(idx);
    }

    PriceLevel& level_at(int32_t tick) { return levels_[tick]; }
    const PriceLevel& level_at(int32_t tick) const { return levels_[tick]; }

    size_t num_ticks() const { return levels_.size(); }

    uint32_t raw_at_tick(int32_t tick) const {
        return min_price_raw_ + static_cast<uint32_t>(tick) * tick_size_raw_;
    }

    double price_at_tick(int32_t tick) const {
        return static_cast<double>(raw_at_tick(tick)) / 10000.0; // ITCH Price(4): 4 implied decimals
    }

private:
    uint32_t min_price_raw_;
    uint32_t tick_size_raw_;
    std::vector<PriceLevel> levels_;
};

} // namespace book
