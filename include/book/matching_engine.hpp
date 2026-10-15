#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "occupied_ticks.hpp"
#include "order_pool.hpp"
#include "price_ladder.hpp"

namespace book {

// A single match between an incoming (taker) order and a resting
// (maker) order, generated during submit_order().
struct Fill {
    uint64_t taker_ref;
    uint64_t maker_ref;
    uint32_t price_raw; // execution price: always the resting (maker) order's price
    uint32_t shares;
};
// Price-time priority matching engine for a single symbol. Incoming
// orders are matched immediately against crossing resting liquidity;
// any unfilled remainder rests in the book at its limit price.
class MatchingEngine {
public:
    MatchingEngine(uint32_t min_price_raw, uint32_t max_price_raw, uint32_t tick_size_raw,
                   size_t max_orders)
        : pool_(max_orders),
          bids_(min_price_raw, max_price_raw, tick_size_raw),
          asks_(min_price_raw, max_price_raw, tick_size_raw),
          bid_ticks_(bids_.num_ticks()),
          ask_ticks_(asks_.num_ticks()) {
        order_locations_.reserve(max_orders);
    }

    // Submits a new order. Matches against crossing resting liquidity
    // first (price-time priority); any unfilled remainder rests in the
    // book. Fills generated are appended to out_fills.
    bool submit_order(uint64_t order_ref, char side, uint32_t price_raw, uint32_t shares,
                       std::vector<Fill>& out_fills) {
        PriceLadder& own_ladder = (side == 'B') ? bids_ : asks_;
        int32_t own_tick = own_ladder.tick_index(price_raw);
        if (own_tick < 0) return false; // price outside configured ladder range

        uint32_t remaining = shares;
        if (side == 'B') {
            match_against_asks(order_ref, price_raw, remaining, out_fills);
        } else {
            match_against_bids(order_ref, price_raw, remaining, out_fills);
        }

        if (remaining > 0) {
            rest_order(order_ref, side, price_raw, remaining);
        }
        return true;
    }

    bool cancel_order(uint64_t order_ref) {
        auto it = order_locations_.find(order_ref);
        if (it == order_locations_.end()) return false;

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

    double best_bid() const {
        return best_bid_tick_ < 0 ? -1.0 : bids_.price_at_tick(best_bid_tick_);
    }
    double best_ask() const {
        return best_ask_tick_ < 0 ? -1.0 : asks_.price_at_tick(best_ask_tick_);
    }
    uint64_t best_bid_shares() const {
        if (best_bid_tick_ < 0) return 0;
        return bids_.level_at(best_bid_tick_).total_shares;
    }
    uint64_t best_ask_shares() const {
        if (best_ask_tick_ < 0) return 0;
        return asks_.level_at(best_ask_tick_).total_shares;
    }
    size_t resting_order_count() const { return order_locations_.size(); }
    uint64_t pool_exhausted() const { return pool_exhausted_; }

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
    OccupiedTicks bid_ticks_;
    OccupiedTicks ask_ticks_;
    int32_t best_bid_tick_ = -1;
    int32_t best_ask_tick_ = -1;
    uint64_t pool_exhausted_ = 0;

    OccupiedTicks& occupied(char side) { return side == 'B' ? bid_ticks_ : ask_ticks_; }

    // Incoming buy at price_raw matches against resting asks at or below
    // price_raw, best (lowest) ask first, FIFO within each price level.
    void match_against_asks(uint64_t taker_ref, uint32_t price_raw, uint32_t& remaining,
                             std::vector<Fill>& out_fills) {
        while (remaining > 0 && best_ask_tick_ >= 0 &&
               asks_.raw_at_tick(best_ask_tick_) <= price_raw) {
            const int32_t tick = best_ask_tick_;
            PriceLevel& level = asks_.level_at(tick);
            fill_against_level(taker_ref, level, remaining, out_fills);
            if (level.order_count == 0) {
                ask_ticks_.clear(tick);
                update_best_on_empty('S', tick);
            }
        }
    }

    // Incoming sell at price_raw matches against resting bids at or
    // above price_raw, best (highest) bid first, FIFO within each level.
    void match_against_bids(uint64_t taker_ref, uint32_t price_raw, uint32_t& remaining,
                             std::vector<Fill>& out_fills) {
        while (remaining > 0 && best_bid_tick_ >= 0 &&
               bids_.raw_at_tick(best_bid_tick_) >= price_raw) {
            const int32_t tick = best_bid_tick_;
            PriceLevel& level = bids_.level_at(tick);
            fill_against_level(taker_ref, level, remaining, out_fills);
            if (level.order_count == 0) {
                bid_ticks_.clear(tick);
                update_best_on_empty('B', tick);
            }
        }
    }

    // Walks a price level's resting orders in FIFO order, generating
    // fills until either `remaining` (taker's unfilled quantity) hits
    // zero or the level is exhausted.
    void fill_against_level(uint64_t taker_ref, PriceLevel& level, uint32_t& remaining,
                             std::vector<Fill>& out_fills) {
        while (remaining > 0 && level.head != kNullIndex) {
            int32_t maker_idx = level.head;
            Order& maker = pool_[maker_idx];

            uint32_t traded = (remaining < maker.shares) ? remaining : maker.shares;
            out_fills.push_back(Fill{taker_ref, maker.order_ref_number, maker.price_raw, traded});

            maker.shares -= traded;
            level.total_shares -= traded;
            remaining -= traded;

            if (maker.shares == 0) {
                remove_from_level(level, maker_idx);
                order_locations_.erase(maker.order_ref_number);
                pool_.free(maker_idx);
            }
        }
    }

    void rest_order(uint64_t order_ref, char side, uint32_t price_raw, uint32_t shares) {
        PriceLadder& ladder = (side == 'B') ? bids_ : asks_;
        int32_t tick = ladder.tick_index(price_raw);

        int32_t idx = pool_.alloc();
        if (idx == kNullIndex) {
            ++pool_exhausted_;
            return;
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

        order_locations_[order_ref] = {idx, tick, side};
        update_best_on_add(side, tick);
    }

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

    void update_best_on_empty(char side, int32_t emptied_tick) {
        if (side == 'B' && emptied_tick == best_bid_tick_) {
            best_bid_tick_ = bid_ticks_.next_below(emptied_tick);
        } else if (side == 'S' && emptied_tick == best_ask_tick_) {
            best_ask_tick_ = ask_ticks_.next_above(emptied_tick);
        }
    }
};

} // namespace book
