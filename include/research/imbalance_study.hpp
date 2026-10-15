#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "linear.hpp"

namespace research {

// ITCH timestamps are nanoseconds since midnight. One second is the
// horizon for the first imbalance study.
inline constexpr uint64_t kForwardHorizonNs = 1'000'000'000ULL;

// Nasdaq regular session in the same clock as ITCH timestamps
// (nanoseconds since midnight). The close is exclusive.
inline constexpr uint64_t kRegularOpenNs = 34'200'000'000'000ULL;  // 09:30
inline constexpr uint64_t kRegularCloseNs = 57'600'000'000'000ULL; // 16:00

// A two-sided book observation. forward_mid stays NaN until a later
// sample at least kForwardHorizonNs ahead is attached.
// bid_gap_ticks / ask_gap_ticks are how many ticks the next occupied
// level sits behind the touch, or -1 when that side has no second level.
// inside_touch is true when a live tick is strictly better than the
// reported best. That is a tracker bug, not a wide market.
struct BookSample {
    uint64_t timestamp_ns = 0;
    double mid = 0;
    double spread = 0;
    double imbalance = 0;
    double microprice = 0;
    double half_spread = 0;
    double forward_mid = std::numeric_limits<double>::quiet_NaN();
    double bid = 0;
    double ask = 0;
    int bid_gap_ticks = -1;
    int ask_gap_ticks = -1;
    bool inside_touch = false;
};

struct DecileRow {
    int bucket = 0; // 0 is the most ask-heavy imbalance
    std::size_t count = 0;
    double mean_change = 0;
    double mean_change_after_cost = 0;
};

inline bool has_forward(const BookSample& sample) {
    return std::isfinite(sample.forward_mid);
}

inline double top_imbalance(uint64_t bid_shares, uint64_t ask_shares) {
    const double denom = static_cast<double>(bid_shares) + static_cast<double>(ask_shares);
    if (denom <= 0.0) return 0.0;
    return (static_cast<double>(bid_shares) - static_cast<double>(ask_shares)) / denom;
}

// Size-weighted mid: the side with more size pulls the microprice toward
// the opposite price, which is where a marketable order would lift.
inline double top_microprice(double bid, double ask, uint64_t bid_shares, uint64_t ask_shares) {
    const double denom = static_cast<double>(bid_shares) + static_cast<double>(ask_shares);
    if (denom <= 0.0) return 0.0;
    return (ask * static_cast<double>(bid_shares) + bid * static_cast<double>(ask_shares)) / denom;
}

// Samples must be in nondecreasing timestamp order. The forward mid is the
// first sample at or after timestamp + horizon, not an interpolated price.
inline void attach_forward_mids(std::vector<BookSample>& samples, uint64_t horizon_ns) {
    const std::size_t n = samples.size();
    std::size_t ahead = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const uint64_t target = samples[i].timestamp_ns + horizon_ns;
        if (target < samples[i].timestamp_ns) {
            samples[i].forward_mid = std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        while (ahead < n && samples[ahead].timestamp_ns < target) ++ahead;
        if (ahead < n) samples[i].forward_mid = samples[ahead].mid;
        else samples[i].forward_mid = std::numeric_limits<double>::quiet_NaN();
    }
}

// Ten equal-count buckets after sorting by imbalance ascending.
// mean_change is forward_mid - mid. The cost term is the sample's half_spread,
// which the collector sets to max(spread, 0) / 2 so a crossed book is not a rebate.
inline std::vector<DecileRow> imbalance_deciles(std::vector<BookSample> samples) {
    std::vector<BookSample> paired;
    paired.reserve(samples.size());
    for (const BookSample& sample : samples) {
        if (has_forward(sample)) paired.push_back(sample);
    }

    std::vector<DecileRow> rows;
    const std::size_t n = paired.size();
    if (n == 0) return rows;

    std::sort(paired.begin(), paired.end(), [](const BookSample& a, const BookSample& b) {
        return a.imbalance < b.imbalance;
    });

    rows.resize(10);
    for (int bucket = 0; bucket < 10; ++bucket) {
        const std::size_t begin = static_cast<std::size_t>(bucket) * n / 10;
        const std::size_t end = static_cast<std::size_t>(bucket + 1) * n / 10;
        double sum_change = 0.0;
        double sum_after = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            const double change = paired[i].forward_mid - paired[i].mid;
            sum_change += change;
            sum_after += change - paired[i].half_spread;
        }
        const std::size_t count = end - begin;
        const double denom = static_cast<double>(count);
        rows[static_cast<std::size_t>(bucket)] = DecileRow{
            bucket,
            count,
            count == 0 ? 0.0 : sum_change / denom,
            count == 0 ? 0.0 : sum_after / denom,
        };
    }
    return rows;
}

struct TouchSummary {
    std::size_t regular_hours_count = 0;
    double median_spread = 0;
    double one_tick_fraction = 0;
    double one_tick_behind_fraction = 0;
    std::size_t inside_touch_count = 0;
};

// One cent. Compared with a tolerance because 0.01 is not a binary fraction.
inline bool is_one_tick_spread(double spread) {
    return std::fabs(spread - 0.01) <= 1e-6;
}

inline bool in_regular_hours(uint64_t timestamp_ns) {
    return timestamp_ns >= kRegularOpenNs && timestamp_ns < kRegularCloseNs;
}

// Regular-hours samples only. one_tick_fraction is the share whose spread
// is one cent. one_tick_behind_fraction is the share whose next occupied
// bid and ask are each exactly one tick behind the touch. A larger gap is
// a hole between the touch and the next live level.
inline TouchSummary summarize_regular_touch(const std::vector<BookSample>& samples) {
    TouchSummary summary;
    std::vector<double> spreads;
    std::size_t one_tick = 0;
    std::size_t one_behind = 0;
    for (const BookSample& sample : samples) {
        if (!in_regular_hours(sample.timestamp_ns)) continue;
        ++summary.regular_hours_count;
        spreads.push_back(sample.spread);
        if (is_one_tick_spread(sample.spread)) ++one_tick;
        if (sample.bid_gap_ticks == 1 && sample.ask_gap_ticks == 1) ++one_behind;
        if (sample.inside_touch) ++summary.inside_touch_count;
    }
    const std::size_t n = summary.regular_hours_count;
    if (n == 0) return summary;

    std::sort(spreads.begin(), spreads.end());
    if (n % 2 == 1) {
        summary.median_spread = spreads[n / 2];
    } else {
        summary.median_spread = (spreads[n / 2 - 1] + spreads[n / 2]) / 2.0;
    }
    const double denom = static_cast<double>(n);
    summary.one_tick_fraction = static_cast<double>(one_tick) / denom;
    summary.one_tick_behind_fraction = static_cast<double>(one_behind) / denom;
    return summary;
}

// A hypothetical decision on a historical sample. It is not a fill:
// the row does not model queue position.
struct DecisionRow {
    uint64_t timestamp_ns = 0;
    char side = '?'; // 'B' buy, 'S' sell
    double bid = 0;
    double ask = 0;
    double spread = 0;
    double imbalance = 0;
    double forward_mid = 0;
    double half_spread = 0;
};

// Outer deciles of regular-hours, one-tick samples that have a forward mid.
// Bucket 0 (most ask-heavy) is a hypothetical sell. Bucket 9 (most bid-heavy)
// is a hypothetical buy. Wide spreads, the premarket, and unpaired samples
// are left out. Joining the queue is not modeled.
inline std::vector<DecisionRow> hypothetical_decisions(std::vector<BookSample> samples) {
    std::vector<BookSample> eligible;
    eligible.reserve(samples.size());
    for (const BookSample& sample : samples) {
        if (!in_regular_hours(sample.timestamp_ns)) continue;
        if (!is_one_tick_spread(sample.spread)) continue;
        if (!has_forward(sample)) continue;
        eligible.push_back(sample);
    }

    std::vector<DecisionRow> rows;
    const std::size_t n = eligible.size();
    if (n == 0) return rows;

    std::sort(eligible.begin(), eligible.end(), [](const BookSample& a, const BookSample& b) {
        return a.imbalance < b.imbalance;
    });

    auto emit = [&](int bucket, char side) {
        const std::size_t begin = static_cast<std::size_t>(bucket) * n / 10;
        const std::size_t end = static_cast<std::size_t>(bucket + 1) * n / 10;
        for (std::size_t i = begin; i < end; ++i) {
            const BookSample& sample = eligible[i];
            rows.push_back(DecisionRow{
                sample.timestamp_ns,
                side,
                sample.bid,
                sample.ask,
                sample.spread,
                sample.imbalance,
                sample.forward_mid,
                sample.half_spread,
            });
        }
    };
    emit(0, 'S');
    emit(9, 'B');
    return rows;
}

struct WalkForwardResult {
    bool ok = false;
    std::size_t train_count = 0;
    std::size_t test_count = 0;
    double intercept = 0;
    double imbalance_slope = 0;
    double spread_slope = 0;
    double in_sample_mean_change = 0;
    double in_sample_mean_after_cost = 0;
    double out_of_sample_mean_change = 0;
    double out_of_sample_mean_after_cost = 0;
};

// Solves M x = b for a 3x3 system. Returns false when the matrix is singular.
inline bool solve_3x3(double matrix[3][3], const double rhs[3], double solution[3]) {
    std::vector<double> a(9);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) a[static_cast<size_t>(i * 3 + j)] = matrix[i][j];
    }
    std::vector<double> x;
    if (!solve_linear(std::move(a), {rhs[0], rhs[1], rhs[2]}, 3, x)) return false;
    solution[0] = x[0];
    solution[1] = x[1];
    solution[2] = x[2];
    return true;
}

// Least squares of (forward_mid - mid) on imbalance and spread.
// The fit uses the first half of regular-hours paired samples in the
// order given, which the collector keeps in time order. The second half
// is only scored. A negative out-of-sample mean after half-spread is
// not a tradable edge. Queue position is not modeled.
inline WalkForwardResult walk_forward_baseline(const std::vector<BookSample>& samples) {
    WalkForwardResult result;
    std::vector<const BookSample*> paired;
    paired.reserve(samples.size());
    for (const BookSample& sample : samples) {
        if (!in_regular_hours(sample.timestamp_ns)) continue;
        if (!has_forward(sample)) continue;
        paired.push_back(&sample);
    }
    const std::size_t n = paired.size();
    const std::size_t train_count = n / 2;
    result.train_count = train_count;
    result.test_count = n - train_count;
    if (train_count < 3) return result;

    double n_train = 0;
    double sum_x1 = 0, sum_x2 = 0, sum_y = 0;
    double sum_x1x1 = 0, sum_x2x2 = 0, sum_x1x2 = 0;
    double sum_x1y = 0, sum_x2y = 0;
    double train_after = 0;
    for (std::size_t i = 0; i < train_count; ++i) {
        const BookSample& sample = *paired[i];
        const double change = sample.forward_mid - sample.mid;
        const double x1 = sample.imbalance;
        const double x2 = sample.spread;
        n_train += 1.0;
        sum_x1 += x1;
        sum_x2 += x2;
        sum_y += change;
        sum_x1x1 += x1 * x1;
        sum_x2x2 += x2 * x2;
        sum_x1x2 += x1 * x2;
        sum_x1y += x1 * change;
        sum_x2y += x2 * change;
        train_after += change - sample.half_spread;
    }

    double matrix[3][3] = {
        {n_train, sum_x1, sum_x2},
        {sum_x1, sum_x1x1, sum_x1x2},
        {sum_x2, sum_x1x2, sum_x2x2},
    };
    const double rhs[3] = {sum_y, sum_x1y, sum_x2y};
    double solution[3] = {};
    if (!solve_3x3(matrix, rhs, solution)) return result;

    result.ok = true;
    result.intercept = solution[0];
    result.imbalance_slope = solution[1];
    result.spread_slope = solution[2];
    result.in_sample_mean_change = sum_y / n_train;
    result.in_sample_mean_after_cost = train_after / n_train;

    if (result.test_count == 0) return result;
    double test_change = 0;
    double test_after = 0;
    for (std::size_t i = train_count; i < n; ++i) {
        const BookSample& sample = *paired[i];
        const double change = sample.forward_mid - sample.mid;
        test_change += change;
        test_after += change - sample.half_spread;
    }
    const double test_n = static_cast<double>(result.test_count);
    result.out_of_sample_mean_change = test_change / test_n;
    result.out_of_sample_mean_after_cost = test_after / test_n;
    return result;
}

} // namespace research
