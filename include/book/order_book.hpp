#pragma once

#include <cstdint>
#include <unordered_map>

#include "occupied_ticks.hpp"
#include "order_pool.hpp"
#include "price_ladder.hpp"

namespace book {

// Why an event did not change the book. Callers that ignore a false return
// without these counts will treat a truncated book as a complete one.
struct DropCounts {
    uint64_t price_out_of_band = 0;
    uint64_t pool_exhausted = 0;
    uint64_t unknown_order = 0;
    uint64_t duplicate_order = 0;
};

// Reconstructs a live limit order book for a single symbol from a stream
// of Add/Cancel/Delete/Replace/Executed events. Bid and ask sides each
// use their own flat-array price ladder; orders are looked up by their
// ITCH order_ref_number via a hash map into the shared OrderPool.
// The map is reserved up front so a rehash cannot stall the hot path.
class OrderBook {
public:
    OrderBook(uint32_t min_price_raw, uint32_t max_price_raw, uint32_t tick_size_raw,
              size_t max_orders)
        : pool_(max_orders),
          bids_(min_price_raw, max_price_raw, tick_size_raw),
          asks_(min_price_raw, max_price_raw, tick_size_raw),
          bid_ticks_(bids_.num_ticks()),
          ask_ticks_(asks_.num_ticks()) {
        order_locations_.reserve(max_orders);
    }

    // Adds a new resting order. Returns false if the pool is exhausted
    // or the price is outside the configured ladder range.
    bool add_order(uint64_t order_ref, char side, uint32_t price_raw, uint32_t shares) {
        if (order_locations_.find(order_ref) != order_locations_.end()) {
            ++drops_.duplicate_order;
            return false;
        }

        PriceLadder& ladder = (side == 'B') ? bids_ : asks_;
        int32_t tick = ladder.tick_index(price_raw);
        if (tick < 0) {
            ++drops_.price_out_of_band;
            return false;
        }

        int32_t idx = pool_.alloc();
        if (idx == kNullIndex) {
            ++drops_.pool_exhausted;
            return false;
        }

        Order& ord = pool_[idx];
        ord.order_ref_number = order_ref;
        ord.side = side;
        ord.price_raw = price_raw;
        ord.shares = shares;

        PriceLevel& level = ladder.level_at(tick);
        const bool was_empty = level.order_count == 0;
        append_to_level(level, idx);
        if (was_empty) occupied(side).set(tick);

        order_locations_[order_ref] = OrderLocation{idx, tick, side};
        update_best_on_add(side, tick);
        return true;
    }

    // Fully cancels an order (ITCH 'D' Order Delete).
    bool delete_order(uint64_t order_ref) {
        auto it = order_locations_.find(order_ref);
        if (it == order_locations_.end()) {
            ++drops_.unknown_order;
            return false;
        }

        OrderLocation loc = it->second;
        PriceLadder& ladder = (loc.side == 'B') ? bids_ : asks_;
        PriceLevel& level = ladder.level_at(loc.tick);

        remove_from_level(level, loc.pool_index);
        pool_.free(loc.pool_index);
        order_locations_.erase(it);

        if (level.order_count == 0) {
            occupied(loc.side).clear(loc.tick);
            update_best_on_empty(loc.side, loc.tick);
        }
        return true;
    }

    // Partially cancels shares from an order (ITCH 'X' Order Cancel).
    bool cancel_shares(uint64_t order_ref, uint32_t cancelled_shares) {
        auto it = order_locations_.find(order_ref);
        if (it == order_locations_.end()) {
            ++drops_.unknown_order;
            return false;
        }

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
            if (level.order_count == 0) {
                occupied(loc.side).clear(loc.tick);
                update_best_on_empty(loc.side, loc.tick);
            }
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
        if (it == order_locations_.end()) {
            ++drops_.unknown_order;
            return false;
        }
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
    size_t ladder_ticks() const { return bids_.num_ticks(); }

    // Shares resting at the current best price, or 0 when that side is empty.
    // This is the size imbalance and microprice read. It is only meaningful
    // because append_to_level adds shares when the order rests.
    uint64_t best_bid_shares() const {
        if (best_bid_tick_ < 0) return 0;
        return bids_.level_at(best_bid_tick_).total_shares;
    }

    uint64_t best_ask_shares() const {
        if (best_ask_tick_ < 0) return 0;
        return asks_.level_at(best_ask_tick_).total_shares;
    }

    // Displayed shares within `ticks` price ticks of the touch, touch
    // included (ticks=5 covers the touch and four ticks behind it). A
    // bitset walk over occupied levels; it does not allocate.
    uint64_t bid_depth_within(int32_t ticks) const {
        uint64_t total = 0;
        for (int32_t t = best_bid_tick_; t >= 0 && best_bid_tick_ - t < ticks;
             t = bid_ticks_.next_below(t)) {
            total += bids_.level_at(t).total_shares;
        }
        return total;
    }

    uint64_t ask_depth_within(int32_t ticks) const {
        uint64_t total = 0;
        for (int32_t t = best_ask_tick_; t >= 0 && t - best_ask_tick_ < ticks;
             t = ask_ticks_.next_above(t)) {
            total += asks_.level_at(t).total_shares;
        }
        return total;
    }

    // Raw fixed-point touch prices, or 0 when the side is empty.
    uint32_t best_bid_raw() const {
        return best_bid_tick_ < 0 ? 0 : bids_.raw_at_tick(best_bid_tick_);
    }
    uint32_t best_ask_raw() const {
        return best_ask_tick_ < 0 ? 0 : asks_.raw_at_tick(best_ask_tick_);
    }

    // Next occupied price behind the touch, or -1 when that side has
    // only one level. These are reads of the existing bitset. They do
    // not allocate.
    double next_bid() const {
        const int32_t tick = next_bid_tick();
        if (tick < 0) return -1.0;
        return tick_to_price(bids_, tick);
    }

    double next_ask() const {
        const int32_t tick = next_ask_tick();
        if (tick < 0) return -1.0;
        return tick_to_price(asks_, tick);
    }

    // Ticks from the touch down to the next bid, or up to the next ask.
    // -1 when there is no second level.
    int next_bid_gap_ticks() const {
        if (best_bid_tick_ < 0) return -1;
        const int32_t next = next_bid_tick();
        if (next < 0) return -1;
        return best_bid_tick_ - next;
    }

    int next_ask_gap_ticks() const {
        if (best_ask_tick_ < 0) return -1;
        const int32_t next = next_ask_tick();
        if (next < 0) return -1;
        return next - best_ask_tick_;
    }

    // True when a bid sits above the reported best bid, or an ask sits
    // below the reported best ask. The touch tracker missed a live level.
    bool touch_has_inside_level() const {
        if (best_bid_tick_ >= 0 && bid_ticks_.next_above(best_bid_tick_) >= 0) return true;
        if (best_ask_tick_ >= 0 && ask_ticks_.next_below(best_ask_tick_) >= 0) return true;
        return false;
    }

    const DropCounts& drop_counts() const { return drops_; }

private:
    OrderPool pool_;
    PriceLadder bids_;
    PriceLadder asks_;
    struct OrderLocation {
        int32_t pool_index;
        int32_t tick;
        char side;
    };
    std::unordered_map<uint64_t, OrderLocation> order_locations_;
    OccupiedTicks bid_ticks_;
    OccupiedTicks ask_ticks_;
    DropCounts drops_;

    OccupiedTicks& occupied(char side) { return side == 'B' ? bid_ticks_ : ask_ticks_; }

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
        level.total_shares += ord.shares;
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

    // The emptied tick's bit is already clear. The next occupied tick is
    // a word scan, which is what removed the gapped-book p99 spike.
    void update_best_on_empty(char side, int32_t emptied_tick) {
        if (side == 'B' && emptied_tick == best_bid_tick_) {
            best_bid_tick_ = bid_ticks_.next_below(emptied_tick);
        } else if (side == 'S' && emptied_tick == best_ask_tick_) {
            best_ask_tick_ = ask_ticks_.next_above(emptied_tick);
        }
    }

    double tick_to_price(const PriceLadder& ladder, int32_t tick) const {
        return ladder.price_at_tick(tick);
    }

    int32_t next_bid_tick() const {
        if (best_bid_tick_ < 0) return -1;
        return bid_ticks_.next_below(best_bid_tick_);
    }

    int32_t next_ask_tick() const {
        if (best_ask_tick_ < 0) return -1;
        return ask_ticks_.next_above(best_ask_tick_);
    }
};

} // namespace book
