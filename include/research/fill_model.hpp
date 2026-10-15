#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace research {

// Hypothetical passive orders at the touch, filled from the tape's own
// queue. Each order is independent: it neither consumes liquidity nor
// changes anyone else's queue. Not modeled: hidden liquidity (P trades do
// not touch the displayed queue), the order's own effect on other traders,
// and latency. Fees are parameters, not Nasdaq's schedule.
struct FillParams {
    uint32_t order_shares = 100;
    uint64_t max_life_ns = 10'000'000'000ULL;
    uint32_t tick_raw = 100;
    uint32_t max_move_ticks = 1;
    double maker_rebate = 0.0;  // per share, credited on a passive fill
    double taker_fee = 0.0;     // per share, charged when crossing
};

// 'Q' queue fill, 'T' trade-through, 'X' expired after max life,
// 'M' touch moved away, 'E' end of data before any outcome.
enum class FillKind : char {
    Open = 'O',
    Queue = 'Q',
    TradeThrough = 'T',
    Expired = 'X',
    Moved = 'M',
    End = 'E',
};

inline bool is_fill(FillKind kind) {
    return kind == FillKind::Queue || kind == FillKind::TradeThrough;
}

constexpr uint64_t kMarkoutHorizonsNs[3] = {1'000'000'000ULL, 5'000'000'000ULL,
                                            30'000'000'000ULL};

struct PassiveOrder {
    size_t sample_index = 0;
    uint64_t entry_ns = 0;
    char side = 'B';
    uint32_t price_raw = 0;
    uint64_t entry_seq = 0;            // tape adds with a lower seq are ahead
    uint64_t queue_ahead_at_entry = 0;
    uint64_t queue_ahead = 0;
    uint64_t behind_executed = 0;      // executions at our price on later orders
    FillKind kind = FillKind::Open;
    uint64_t done_ns = 0;
    // Signed mid minus fill price (bid) or fill price minus mid (ask) at
    // +1s, +5s, +30s after the fill, per share. NaN when unfilled or past
    // the end of the data.
    double markout[3] = {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::quiet_NaN()};

    double fill_price() const { return static_cast<double>(price_raw) / 10000.0; }
    uint64_t time_to_fill_ns() const { return is_fill(kind) ? done_ns - entry_ns : 0; }
};

class FillSimulator {
public:
    explicit FillSimulator(FillParams params, size_t reserve_refs = 0) : params_(params) {
        refs_.reserve(reserve_refs);
    }

    // Tape events for the study's symbol, in tape order, before on_touch.
    void on_add(uint64_t ref, char side, uint32_t price_raw, uint32_t shares) {
        refs_[ref] = RefInfo{next_seq_++, price_raw, shares, side};
    }

    void on_execute(uint64_t ts, uint64_t ref, uint32_t shares) { reduce(ts, ref, shares, true); }

    void on_cancel(uint64_t ts, uint64_t ref, uint32_t shares) { reduce(ts, ref, shares, false); }

    void on_delete(uint64_t ts, uint64_t ref) {
        auto it = refs_.find(ref);
        if (it == refs_.end()) return;
        reduce(ts, ref, it->second.remaining, false);
    }

    // ITCH U: the new reference loses time priority.
    void on_replace(uint64_t ts, uint64_t old_ref, uint64_t new_ref, uint32_t price_raw,
                    uint32_t shares) {
        auto it = refs_.find(old_ref);
        if (it == refs_.end()) return;
        const char side = it->second.side;
        on_delete(ts, old_ref);
        on_add(new_ref, side, price_raw, shares);
    }

    // After every event: trade-through, then expiry. 0 means the side is empty.
    void on_touch(uint64_t ts, uint32_t best_bid_raw, uint32_t best_ask_raw) {
        for (size_t i = 0; i < active_.size();) {
            PassiveOrder& o = orders_[active_[i]];
            const uint64_t away = static_cast<uint64_t>(params_.tick_raw) * params_.max_move_ticks;
            if (o.side == 'B' && best_ask_raw != 0 && best_ask_raw <= o.price_raw) {
                close(o, FillKind::TradeThrough, ts);
            } else if (o.side == 'S' && best_bid_raw != 0 && best_bid_raw >= o.price_raw) {
                close(o, FillKind::TradeThrough, ts);
            } else if (ts >= o.entry_ns + params_.max_life_ns) {
                close(o, FillKind::Expired, ts);
            } else if (o.side == 'B' && best_bid_raw != 0 &&
                       best_bid_raw > o.price_raw + away) {
                close(o, FillKind::Moved, ts);
            } else if (o.side == 'S' && best_ask_raw != 0 &&
                       static_cast<uint64_t>(best_ask_raw) + away < o.price_raw) {
                close(o, FillKind::Moved, ts);
            }
            if (o.kind != FillKind::Open) {
                active_[i] = active_.back();
                active_.pop_back();
            } else {
                ++i;
            }
        }
    }

    // Joins the back of the queue at price_raw. queue_ahead is the displayed
    // shares resting there now. Returns the order's index.
    size_t place(size_t sample_index, uint64_t ts, char side, uint32_t price_raw,
                 uint64_t queue_ahead) {
        PassiveOrder o;
        o.sample_index = sample_index;
        o.entry_ns = ts;
        o.side = side;
        o.price_raw = price_raw;
        o.entry_seq = next_seq_;
        o.queue_ahead_at_entry = queue_ahead;
        o.queue_ahead = queue_ahead;
        orders_.push_back(o);
        active_.push_back(orders_.size() - 1);
        return orders_.size() - 1;
    }

    void finish(uint64_t ts) {
        for (const size_t idx : active_) close(orders_[idx], FillKind::End, ts);
        active_.clear();
    }

    // Side of a live tape order, or 0 if it is unknown or gone.
    char side_of(uint64_t ref) const {
        auto it = refs_.find(ref);
        return it == refs_.end() ? 0 : it->second.side;
    }

    const std::vector<PassiveOrder>& orders() const { return orders_; }
    std::vector<PassiveOrder>& orders() { return orders_; }
    size_t active_count() const { return active_.size(); }
    const FillParams& params() const { return params_; }

private:
    struct RefInfo {
        uint64_t seq;
        uint32_t price_raw;
        uint32_t remaining;
        char side;
    };

    FillParams params_;
    std::unordered_map<uint64_t, RefInfo> refs_;
    std::vector<PassiveOrder> orders_;
    std::vector<size_t> active_;
    uint64_t next_seq_ = 0;

    static void close(PassiveOrder& o, FillKind kind, uint64_t ts) {
        o.kind = kind;
        o.done_ns = ts;
    }

    void reduce(uint64_t ts, uint64_t ref, uint32_t shares, bool executed) {
        auto it = refs_.find(ref);
        if (it == refs_.end()) return;
        RefInfo& info = it->second;
        const uint32_t applied = std::min(shares, info.remaining);
        for (size_t i = 0; i < active_.size();) {
            PassiveOrder& o = orders_[active_[i]];
            if (o.side == info.side && o.price_raw == info.price_raw) {
                if (info.seq < o.entry_seq) {
                    o.queue_ahead -= std::min<uint64_t>(applied, o.queue_ahead);
                } else if (executed) {
                    // Price-time priority: a later order at our price only
                    // trades after us, so this volume would have hit us first.
                    o.behind_executed += applied;
                    if (o.behind_executed >= params_.order_shares) {
                        close(o, FillKind::Queue, ts);
                        active_[i] = active_.back();
                        active_.pop_back();
                        continue;
                    }
                }
            }
            ++i;
        }
        info.remaining -= applied;
        if (info.remaining == 0) refs_.erase(it);
    }
};

// Markouts from a time-sorted mid series: the first mid at or after
// fill + horizon. Bid: mid - price. Ask: price - mid. Rebate not included.
inline void attach_markouts(std::vector<PassiveOrder>& orders,
                            const std::vector<uint64_t>& mid_ts,
                            const std::vector<double>& mids) {
    for (PassiveOrder& o : orders) {
        if (!is_fill(o.kind)) continue;
        for (int h = 0; h < 3; ++h) {
            const uint64_t target = o.done_ns + kMarkoutHorizonsNs[h];
            const auto it = std::lower_bound(mid_ts.begin(), mid_ts.end(), target);
            if (it == mid_ts.end()) continue;
            const double mid = mids[static_cast<size_t>(it - mid_ts.begin())];
            o.markout[h] = (o.side == 'B') ? mid - o.fill_price() : o.fill_price() - mid;
        }
    }
}

} // namespace research
