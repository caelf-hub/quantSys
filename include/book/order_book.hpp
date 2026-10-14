#pragma once

#include <cstdint>
#include <unordered_map>

#include "order_pool.hpp"
#include "price_ladder.hpp"

namespace book {

// Reconstructs a live limit order book for a single symbol from a stream
// of Add/Cancel/Delete/Replace/Executed events. Bid and ask sides each
// use their own flat-array price ladder; orders are looked up by their
// ITCH order_ref_number via a hash map into the shared OrderPool.
class OrderBook {
public:
    OrderBook(uint32_t min_price_raw, uint32_t max_price_raw, uint32_t tick_size_raw,
              size_t max_orders)
        : pool_(max_orders),
          bids_(min_price_raw, max_price_raw, tick_size_raw),
          asks_(min_price_raw, max_price_raw, tick_size_raw) {}

    // Adds a new resting order. Returns false if the pool is exhausted
    // or the price is outside the configured ladder range.
    bool add_order(uint64_t order_ref, char side, uint32_t price_raw, uint32_t shares) {
        PriceLadder& ladder = (side == 'B') ? bids_ : asks_;
        int32_t tick = ladder.tick_index(price_raw);
        if (tick < 0) return false;

        int32_t idx = pool_.alloc();
        if (idx == kNullIndex) return false;

        Order& ord = pool_[idx];
        ord.order_ref_number = order_ref;
        ord.side = side;
        ord.price_raw = price_raw;
        ord.shares = shares;

        PriceLevel& level = ladder.level_at(tick);
        append_to_level(level, idx);

        order_locations_[order_ref] = {idx, tick, side};
        update_best_on_add(side, tick);
        return true;
    }

    // Fully cancels an order (ITCH 'D' Order Delete).
    bool delete_order(uint64_t order_ref) {
        auto it = order_locations_.find(order_ref);
        if (it == order_locations_.end()) return false;

        OrderLocation loc = it->second;
        PriceLadder& ladder = (loc.side == 'B') ? bids_ : asks_;
        PriceLevel& level = ladder.level_at(loc.tick);

        remove_from_level(level, loc.pool_index);
        pool_.free(loc.pool_index);
        order_locations_.erase(it);

        if (level.order_count == 0) update_best_on_empty(loc.side, loc.tick);
        return true;
    }

    // Partially cancels shares from an order (ITCH 'X' Order Cancel).
    bool cancel_shares(uint64_t order_ref, uint32_t cancelled_shares) {
        auto it = order_locations_.find(order_ref);
        if (it == order_locations_.end()) return false;

        OrderLocation loc = it->second;
        Order& ord = pool_[loc.pool_index];
        PriceLadder& ladder = (loc.side == 'B') ? bids_ : asks_;
        PriceLevel& level = ladder.level_at(loc.tick);

        uint32_t applied = (cancelled_shares <= ord.shares) ? cancelled_shares : ord.shares;
        ord.shares -= applied;
        level.total_shares -= applied;

        if (ord.shares == 0) {
            remove_from_level(level, loc.pool_index);
            pool_.free(loc.pool_index);
            order_locations_.erase(it);
            if (level.order_count == 0) update_best_on_empty(loc.side, loc.tick);
        }
        return true;
    }

    // Applies an execution against an order (ITCH 'E'/'C' Order Executed).
    bool execute_shares(uint64_t order_ref, uint32_t executed_shares) {
        return cancel_shares(order_ref, executed_shares); // same bookkeeping effect
    }

    // Cancel-replace (ITCH 'U'): remove the old order, add a new one at
    // the given price/shares under a new reference number.
    bool replace_order(uint64_t old_order_ref, uint64_t new_order_ref,
                        uint32_t new_price_raw, uint32_t new_shares) {
        auto it = order_locations_.find(old_order_ref);
        if (it == order_locations_.end()) return false;
        char side = it->second.side;

        if (!delete_order(old_order_ref)) return false;
        return add_order(new_order_ref, side, new_price_raw, new_shares);
    }

    double best_bid() const {
        if (best_bid_tick_ < 0) return -1.0;
        return tick_to_price(bids_, best_bid_tick_);
    }

    double best_ask() const {
        if (best_ask_tick_ < 0) return -1.0;
        return tick_to_price(asks_, best_ask_tick_);
    }

    size_t resting_order_count() const { return order_locations_.size(); }

private:
    struct OrderLocation {
        int32_t pool_index;
        int32_t tick;
        char side;
    };

    OrderPool pool_;
    PriceLadder bids_;
    PriceLadder asks_;
    std::unordered_map<uint64_t, OrderLocation> order_locations_;

    int32_t best_bid_tick_ = -1; // highest occupied bid tick
    int32_t best_ask_tick_ = -1; // lowest occupied ask tick

    void append_to_level(PriceLevel& level, int32_t idx) {
        Order& ord = pool_[idx];
        ord.prev = level.tail;
        ord.next = kNullIndex;
        if (level.tail != kNullIndex) {
            pool_[level.tail].next = idx;
        } else {
            level.head = idx;
        }
        level.tail = idx;
        level.order_count++;
    }

    void remove_from_level(PriceLevel& level, int32_t idx) {
        Order& ord = pool_[idx];
        if (ord.prev != kNullIndex) pool_[ord.prev].next = ord.next;
        else level.head = ord.next;

        if (ord.next != kNullIndex) pool_[ord.next].prev = ord.prev;
        else level.tail = ord.prev;

        level.total_shares -= ord.shares;
        level.order_count--;
    }

    void update_best_on_add(char side, int32_t tick) {
        if (side == 'B') {
            if (best_bid_tick_ < 0 || tick > best_bid_tick_) best_bid_tick_ = tick;
        } else {
            if (best_ask_tick_ < 0 || tick < best_ask_tick_) best_ask_tick_ = tick;
        }
    }

    // Called when a price level empties. If it was the best level, scan
    // outward for the next occupied level. Documented O(ticks) worst case;
    // a bitset of occupied ticks would make this O(1) amortized if profiling
    // shows this matters.
    void update_best_on_empty(char side, int32_t emptied_tick) {
        if (side == 'B' && emptied_tick == best_bid_tick_) {
            int32_t t = emptied_tick - 1;
            while (t >= 0 && bids_.level_at(t).order_count == 0) --t;
            best_bid_tick_ = (t >= 0) ? t : -1;
        } else if (side == 'S' && emptied_tick == best_ask_tick_) {
            size_t n = asks_.num_ticks();
            int32_t t = emptied_tick + 1;
            while (static_cast<size_t>(t) < n && asks_.level_at(t).order_count == 0) ++t;
            best_ask_tick_ = (static_cast<size_t>(t) < n) ? t : -1;
        }
    }

    double tick_to_price(const PriceLadder& ladder, int32_t tick) const {
        return ladder.price_at_tick(tick);
    }
};

} // namespace book
