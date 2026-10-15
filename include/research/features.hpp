#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace research {

constexpr uint64_t kFeatureWindowNs = 1'000'000'000ULL;
constexpr uint64_t kTargetHorizonsNs[3] = {1'000'000'000ULL, 5'000'000'000ULL,
                                           30'000'000'000ULL};
constexpr int kDepthTicks = 5;

// Sum of integer values over (now - window, now]. Times must not go
// backwards. Old entries are compacted, so memory follows the window.
class RollingSum {
public:
    explicit RollingSum(uint64_t window_ns = kFeatureWindowNs) : window_(window_ns) {}

    void add(uint64_t ts, int64_t value) {
        expire(ts);
        buf_.emplace_back(ts, value);
        sum_ += value;
    }

    int64_t sum(uint64_t now) {
        expire(now);
        return sum_;
    }

private:
    uint64_t window_;
    std::vector<std::pair<uint64_t, int64_t>> buf_;
    size_t head_ = 0;
    int64_t sum_ = 0;

    void expire(uint64_t now) {
        while (head_ < buf_.size() && buf_[head_].first + window_ <= now) {
            sum_ -= buf_[head_].second;
            ++head_;
        }
        if (head_ > 4096 && head_ * 2 > buf_.size()) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
    }
};

struct TouchState {
    uint32_t bid_raw = 0;  // 0 when that side is empty
    uint32_t ask_raw = 0;
    uint64_t bid_shares = 0;
    uint64_t ask_shares = 0;

    bool two_sided() const { return bid_raw != 0 && ask_raw != 0; }
    bool operator==(const TouchState& o) const {
        return bid_raw == o.bid_raw && ask_raw == o.ask_raw && bid_shares == o.bid_shares &&
               ask_shares == o.ask_shares;
    }
};

// Order-flow imbalance increment at the touch, Cont, Kukanov, and Stoikov
// (2014), eq. 2: bid-side flow minus ask-side flow, in shares.
inline int64_t ofi_increment(const TouchState& prev, const TouchState& now) {
    int64_t e = 0;
    if (now.bid_raw >= prev.bid_raw) e += static_cast<int64_t>(now.bid_shares);
    if (now.bid_raw <= prev.bid_raw) e -= static_cast<int64_t>(prev.bid_shares);
    if (now.ask_raw <= prev.ask_raw) e -= static_cast<int64_t>(now.ask_shares);
    if (now.ask_raw >= prev.ask_raw) e += static_cast<int64_t>(prev.ask_shares);
    return e;
}

// Streaming inputs that need the event history. Feed every event for the
// symbol in tape order, then read at the sample time.
class FeatureTracker {
public:
    // Call after each book update with the new touch.
    void on_touch(uint64_t ts, const TouchState& now) {
        if (have_prev_ && prev_.two_sided() && now.two_sided() && !(now == prev_)) {
            ofi_.add(ts, ofi_increment(prev_, now));
        }
        prev_ = now;
        have_prev_ = true;
    }

    // An execution against a displayed resting order, or a P trade with its
    // indicator. A resting bid filled means a seller crossed: negative.
    void on_execution(uint64_t ts, char resting_side, uint32_t shares) {
        if (resting_side != 'B' && resting_side != 'S') return;
        signed_volume_.add(ts, resting_side == 'S' ? static_cast<int64_t>(shares)
                                                   : -static_cast<int64_t>(shares));
    }

    int64_t ofi_1s(uint64_t now) { return ofi_.sum(now); }
    int64_t signed_volume_1s(uint64_t now) { return signed_volume_.sum(now); }

private:
    RollingSum ofi_;
    RollingSum signed_volume_;
    TouchState prev_;
    bool have_prev_ = false;
};

struct FeatureRow {
    uint64_t timestamp_ns = 0;
    double mid = 0.0;
    double spread = 0.0;
    double imbalance = 0.0;
    double micro_minus_mid = 0.0;
    double ofi_1s = 0.0;
    double depth5_imbalance = 0.0;
    double signed_volume_1s = 0.0;
    double mid_change_1s = 0.0;
    double minutes_since_open = 0.0;
    double target[3] = {std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::quiet_NaN()};
};

inline double depth_imbalance(uint64_t bid_depth, uint64_t ask_depth) {
    const uint64_t total = bid_depth + ask_depth;
    if (total == 0) return 0.0;
    return (static_cast<double>(bid_depth) - static_cast<double>(ask_depth)) /
           static_cast<double>(total);
}

inline double minutes_since(uint64_t ts, uint64_t open_ns) {
    return (static_cast<double>(ts) - static_cast<double>(open_ns)) / 60e9;
}

// Mid change from the latest earlier row at least `lag_ns` before row idx.
// Uses rows [0, idx] only. 0 when there is no such row.
inline double lagged_mid_change(const std::vector<FeatureRow>& rows, size_t idx,
                                uint64_t lag_ns) {
    const uint64_t now = rows[idx].timestamp_ns;
    if (now < lag_ns) return 0.0;
    const uint64_t cutoff = now - lag_ns;
    const auto end = rows.begin() + static_cast<std::ptrdiff_t>(idx) + 1;
    auto it = std::upper_bound(rows.begin(), end, cutoff,
                               [](uint64_t t, const FeatureRow& r) { return t < r.timestamp_ns; });
    if (it == rows.begin()) return 0.0;
    --it;
    return rows[idx].mid - it->mid;
}

// Forward mid change to the first row at or after ts + horizon. NaN when
// the data ends first. Rows must be time-sorted.
inline void attach_targets(std::vector<FeatureRow>& rows) {
    for (FeatureRow& row : rows) {
        for (int h = 0; h < 3; ++h) {
            const uint64_t target = row.timestamp_ns + kTargetHorizonsNs[h];
            auto it = std::lower_bound(
                rows.begin(), rows.end(), target,
                [](const FeatureRow& r, uint64_t t) { return r.timestamp_ns < t; });
            if (it == rows.end()) continue;
            row.target[h] = it->mid - row.mid;
        }
    }
}

} // namespace research
