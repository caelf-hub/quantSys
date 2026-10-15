#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "linear.hpp"

namespace research {

constexpr size_t kEvalFeatures = 8;
constexpr double kEvalLambdas[] = {0.0, 1e-4, 1e-2, 1.0, 10.0, 100.0};
constexpr uint64_t kBootstrapSeed = 1;
constexpr size_t kBootstrapDraws = 1000;

inline constexpr const char* kEvalFeatureNames[kEvalFeatures] = {
    "imbalance",        "micro_minus_mid", "spread",         "ofi_1s",
    "depth5_imbalance", "signed_volume_1s", "mid_change_1s", "minutes_since_open",
};

struct EvalRow {
    uint64_t timestamp_ns = 0;
    double x[kEvalFeatures] = {};
    double target = std::numeric_limits<double>::quiet_NaN();
    double half_spread = 0.0;
    char bid_kind = 0;
    char ask_kind = 0;
    double bid_markout = std::numeric_limits<double>::quiet_NaN();
    double ask_markout = std::numeric_limits<double>::quiet_NaN();
};

struct EvalDay {
    std::string label;
    std::string path;
    std::vector<EvalRow> rows;
};

struct Scale {
    double mean[kEvalFeatures] = {};
    double std[kEvalFeatures] = {};
};

struct Fit {
    std::vector<double> beta;  // intercept, then kEvalFeatures
    Scale scale;
    double lambda = 0.0;
    bool ok = false;
};

struct RuleStats {
    uint64_t n = 0;
    uint64_t trades = 0;
    uint64_t hits = 0;
    uint64_t passive_placed = 0;
    uint64_t passive_filled = 0;
    double sum_pred = 0.0;
    double sum_target = 0.0;
    double sum_pred2 = 0.0;
    double sum_target2 = 0.0;
    double sum_pred_target = 0.0;
    double sum_after_cost = 0.0;
    double sum_passive = 0.0;

    double corr() const {
        if (n < 2) return 0.0;
        const double nn = static_cast<double>(n);
        const double num = nn * sum_pred_target - sum_pred * sum_target;
        const double denx = nn * sum_pred2 - sum_pred * sum_pred;
        const double deny = nn * sum_target2 - sum_target * sum_target;
        if (denx <= 0.0 || deny <= 0.0) return 0.0;
        return num / std::sqrt(denx * deny);
    }
    double mean_after_cost() const {
        return trades == 0 ? 0.0 : sum_after_cost / static_cast<double>(trades);
    }
    // Includes rows with no trade as zero. Per-trade mean can look worse
    // only because the rule traded more.
    double mean_per_sample() const {
        return n == 0 ? 0.0 : sum_after_cost / static_cast<double>(n);
    }
    double hit_rate() const {
        return trades == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(trades);
    }
    double passive_fill_rate() const {
        return passive_placed == 0
                   ? 0.0
                   : static_cast<double>(passive_filled) / static_cast<double>(passive_placed);
    }
    double mean_passive() const {
        return passive_filled == 0 ? 0.0 : sum_passive / static_cast<double>(passive_filled);
    }
};

struct DayResult {
    std::string day;
    int horizon_s = 0;
    uint64_t n = 0;
    double corr = 0.0;
    double corr_thinned = 0.0;
    uint64_t ridge_trades = 0;
    double ridge_mean_after_cost = 0.0;
    double ridge_mean_per_sample = 0.0;
    double ridge_hit_rate = 0.0;
    uint64_t one_tick_n = 0;
    uint64_t one_tick_trades = 0;
    double one_tick_mean_after_cost = 0.0;
    double one_tick_mean_per_sample = 0.0;
    uint64_t passive_placed = 0;
    uint64_t passive_filled = 0;
    double passive_fill_rate = 0.0;
    double passive_mean = 0.0;
    double imbalance_mean_after_cost = 0.0;
    uint64_t decile_trades = 0;
    double decile_mean_after_cost = 0.0;
    double ablation_mean[kEvalFeatures] = {};
    Fit fit;
};

struct Interval {
    double mean = 0.0;
    double lo = 0.0;
    double hi = 0.0;
};

struct EvalSummary {
    int horizon_s = 0;
    std::vector<DayResult> days;
    Interval ridge;
    Interval ridge_per_sample;
    Interval one_tick;
    Interval imbalance;
    Interval decile;
    Interval passive;
    Interval ablation[kEvalFeatures];
    double mean_std_beta[kEvalFeatures] = {};
    double mean_corr_thinned = 0.0;
};

inline double parse_cell(std::string_view cell) {
    if (cell.empty()) return std::numeric_limits<double>::quiet_NaN();
    double value = 0.0;
    const auto parsed = std::from_chars(cell.data(), cell.data() + cell.size(), value);
    if (parsed.ec != std::errc{}) return std::numeric_limits<double>::quiet_NaN();
    return value;
}

inline std::string day_label_from_path(const std::string& path) {
    std::filesystem::path p(path);
    const std::string parent = p.parent_path().filename().string();
    if (parent.size() == 8 && std::all_of(parent.begin(), parent.end(),
                                          [](unsigned char c) { return std::isdigit(c); })) {
        return parent;
    }
    return p.stem().string();
}

inline bool load_features_csv(const std::string& path, int horizon, std::vector<EvalRow>& out,
                              std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    if (horizon < 0 || horizon > 2) {
        if (error) *error = "horizon must be 0, 1, or 2";
        return false;
    }
    std::string header;
    if (!std::getline(in, header)) {
        if (error) *error = "empty file " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cells;
        std::string cell;
        std::istringstream ss(line);
        while (std::getline(ss, cell, ',')) cells.push_back(cell);
        // 13 feature/target columns plus optional fill-model tails.
        if (cells.size() < 13) continue;
        cells.resize(25);
        EvalRow row;
        {
            uint64_t ts = 0;
            const auto parsed =
                std::from_chars(cells[0].data(), cells[0].data() + cells[0].size(), ts);
            if (parsed.ec == std::errc{}) row.timestamp_ns = ts;
        }
        row.x[0] = parse_cell(cells[3]);
        row.x[1] = parse_cell(cells[4]);
        row.x[2] = parse_cell(cells[2]);
        row.x[3] = parse_cell(cells[5]);
        row.x[4] = parse_cell(cells[6]);
        row.x[5] = parse_cell(cells[7]);
        row.x[6] = parse_cell(cells[8]);
        row.x[7] = parse_cell(cells[9]);
        row.target = parse_cell(cells[static_cast<size_t>(10 + horizon)]);
        const double spread = parse_cell(cells[2]);
        row.half_spread = std::isfinite(spread) ? std::max(spread, 0.0) / 2.0 : 0.0;
        if (!cells[13].empty()) row.bid_kind = cells[13][0];
        row.bid_markout = parse_cell(cells[static_cast<size_t>(16 + horizon)]);
        if (!cells[19].empty()) row.ask_kind = cells[19][0];
        row.ask_markout = parse_cell(cells[static_cast<size_t>(22 + horizon)]);
        if (!std::isfinite(row.target)) continue;
        for (size_t i = 0; i < kEvalFeatures; ++i) {
            if (!std::isfinite(row.x[i])) row.x[i] = 0.0;
        }
        out.push_back(row);
    }
    return true;
}

inline Scale scale_from(const std::vector<const EvalRow*>& rows) {
    Scale s;
    const double n = static_cast<double>(rows.size());
    if (n <= 0.0) return s;
    for (const EvalRow* r : rows) {
        for (size_t i = 0; i < kEvalFeatures; ++i) s.mean[i] += r->x[i];
    }
    for (size_t i = 0; i < kEvalFeatures; ++i) s.mean[i] /= n;
    for (const EvalRow* r : rows) {
        for (size_t i = 0; i < kEvalFeatures; ++i) {
            const double d = r->x[i] - s.mean[i];
            s.std[i] += d * d;
        }
    }
    for (size_t i = 0; i < kEvalFeatures; ++i) {
        s.std[i] = std::sqrt(s.std[i] / n);
        if (s.std[i] < 1e-12) s.std[i] = 0.0;
    }
    return s;
}

inline double z(const Scale& s, const EvalRow& r, size_t i) {
    if (s.std[i] <= 0.0) return 0.0;
    return (r.x[i] - s.mean[i]) / s.std[i];
}

inline double predict(const Fit& fit, const EvalRow& r) {
    double y = fit.beta[0];
    for (size_t i = 0; i < kEvalFeatures && i + 1 < fit.beta.size(); ++i) {
        y += fit.beta[i + 1] * z(fit.scale, r, i);
    }
    return y;
}

// Unstandardized weight on feature i: beta_std[i+1] / std[i].
inline double original_weight(const Fit& fit, size_t feature) {
    if (!fit.ok || feature + 1 >= fit.beta.size() || fit.scale.std[feature] <= 0.0) return 0.0;
    return fit.beta[feature + 1] / fit.scale.std[feature];
}

inline bool fit_ridge(const std::vector<const EvalRow*>& rows, const Scale& scale, double lambda,
                      const bool used[kEvalFeatures], Fit& out) {
    size_t keep[kEvalFeatures];
    size_t m = 0;
    for (size_t i = 0; i < kEvalFeatures; ++i) {
        if (used[i] && scale.std[i] > 0.0) keep[m++] = i;
    }
    const size_t k = m + 1;
    std::vector<double> a(k * k, 0.0);
    std::vector<double> b(k, 0.0);
    for (const EvalRow* r : rows) {
        std::vector<double> v(k);
        v[0] = 1.0;
        for (size_t j = 0; j < m; ++j) v[j + 1] = z(scale, *r, keep[j]);
        for (size_t i = 0; i < k; ++i) {
            b[i] += v[i] * r->target;
            for (size_t j = 0; j < k; ++j) a[i * k + j] += v[i] * v[j];
        }
    }
    for (size_t i = 1; i < k; ++i) a[i * k + i] += lambda;
    std::vector<double> x;
    if (!solve_linear(a, b, k, x)) return false;
    out.beta.assign(kEvalFeatures + 1, 0.0);
    out.beta[0] = x[0];
    for (size_t j = 0; j < m; ++j) out.beta[keep[j] + 1] = x[j + 1];
    out.scale = scale;
    out.lambda = lambda;
    out.ok = true;
    return true;
}

inline double mse(const Fit& fit, const std::vector<const EvalRow*>& rows) {
    if (rows.empty()) return std::numeric_limits<double>::infinity();
    double s = 0.0;
    for (const EvalRow* r : rows) {
        const double e = predict(fit, *r) - r->target;
        s += e * e;
    }
    return s / static_cast<double>(rows.size());
}

inline double choose_lambda(const std::vector<const EvalRow*>& train,
                            const std::vector<const EvalRow*>& val, const Scale& scale,
                            const bool used[kEvalFeatures]) {
    double best_lambda = kEvalLambdas[0];
    double best = std::numeric_limits<double>::infinity();
    for (double lambda : kEvalLambdas) {
        Fit fit;
        if (!fit_ridge(train, scale, lambda, used, fit)) continue;
        const double err = val.empty() ? 0.0 : mse(fit, val);
        if (err < best) {
            best = err;
            best_lambda = lambda;
        }
    }
    return best_lambda;
}

inline void note_pred(RuleStats& s, double pred, double target) {
    ++s.n;
    s.sum_pred += pred;
    s.sum_target += target;
    s.sum_pred2 += pred * pred;
    s.sum_target2 += target * target;
    s.sum_pred_target += pred * target;
}

inline void note_trade(RuleStats& s, double pnl) {
    ++s.trades;
    s.sum_after_cost += pnl;
    if (pnl > 0.0) ++s.hits;
}

inline bool filled_kind(char k) { return k == 'Q' || k == 'T'; }

// Cross when |pred| exceeds half-spread plus taker fee. PnL is signed mid
// change minus that cost. A row with no trade is not a hit or a miss.
inline void score_cross(RuleStats& s, double pred, const EvalRow& r, double taker_fee) {
    note_pred(s, pred, r.target);
    const double cost = r.half_spread + taker_fee;
    if (pred > cost) {
        note_trade(s, r.target - cost);
    } else if (pred < -cost) {
        note_trade(s, -r.target - cost);
    }
}

inline void score_passive(RuleStats& s, double pred, const EvalRow& r, double maker_rebate) {
    if (pred == 0.0) return;
    const bool buy = pred > 0.0;
    const char kind = buy ? r.bid_kind : r.ask_kind;
    if (kind == 0) return;
    ++s.passive_placed;
    if (!filled_kind(kind)) return;
    ++s.passive_filled;
    const double mark = buy ? r.bid_markout : r.ask_markout;
    if (std::isfinite(mark)) s.sum_passive += mark + maker_rebate;
}

// Train-only outer-decile edges of imbalance. q_low is the largest value in
// bucket 0; q_high is the smallest in bucket 9.
inline bool train_decile_edges(const std::vector<const EvalRow*>& train, double& q_low,
                               double& q_high) {
    std::vector<double> imb;
    imb.reserve(train.size());
    for (const EvalRow* r : train) imb.push_back(r->x[0]);
    const size_t n = imb.size();
    if (n < 10) return false;
    std::sort(imb.begin(), imb.end());
    const size_t end0 = n / 10;
    const size_t begin9 = 9 * n / 10;
    if (end0 == 0 || begin9 >= n) return false;
    q_low = imb[end0 - 1];
    q_high = imb[begin9];
    return true;
}

inline void score_decile(RuleStats& s, const EvalRow& r, double q_low, double q_high,
                         double taker_fee) {
    const double cost = r.half_spread + taker_fee;
    if (r.x[0] <= q_low) {
        note_trade(s, -r.target - cost);
    } else if (r.x[0] >= q_high) {
        note_trade(s, r.target - cost);
    }
}

inline bool one_tick_spread(const EvalRow& r) { return std::fabs(r.x[2] - 0.01) <= 1e-6; }

// Keep a sample only if it is at least min_gap_ns after the previous kept
// timestamp. 100 ms overlapping rows inflate correlation.
inline size_t thin_indices(const std::vector<EvalRow>& rows, uint64_t min_gap_ns,
                           std::vector<size_t>& keep) {
    keep.clear();
    uint64_t next = 0;
    bool have = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!have || rows[i].timestamp_ns >= next) {
            keep.push_back(i);
            next = rows[i].timestamp_ns + min_gap_ns;
            have = true;
        }
    }
    return keep.size();
}

inline std::vector<const EvalRow*> pointers(const std::vector<EvalRow>& rows) {
    std::vector<const EvalRow*> p;
    p.reserve(rows.size());
    for (const EvalRow& r : rows) p.push_back(&r);
    return p;
}

inline Interval bootstrap_mean(const std::vector<double>& values, uint64_t seed) {
    Interval iv;
    if (values.empty()) return iv;
    const double n = static_cast<double>(values.size());
    iv.mean = std::accumulate(values.begin(), values.end(), 0.0) / n;
    if (values.size() == 1) {
        iv.lo = iv.hi = iv.mean;
        return iv;
    }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, values.size() - 1);
    std::vector<double> means;
    means.reserve(kBootstrapDraws);
    for (size_t b = 0; b < kBootstrapDraws; ++b) {
        double sum = 0.0;
        for (size_t i = 0; i < values.size(); ++i) sum += values[pick(rng)];
        means.push_back(sum / n);
    }
    std::sort(means.begin(), means.end());
    const size_t lo = static_cast<size_t>(0.025 * (kBootstrapDraws - 1));
    const size_t hi = static_cast<size_t>(0.975 * (kBootstrapDraws - 1));
    iv.lo = means[lo];
    iv.hi = means[hi];
    return iv;
}

// Leave-one-day-out. Inner hold-out is the last remaining training day
// (chronological in the input order). The held-out day is never in its fit.
inline EvalSummary evaluate_loo(const std::vector<EvalDay>& days, int horizon_s, double taker_fee,
                                double maker_rebate, uint64_t seed = kBootstrapSeed) {
    EvalSummary summary;
    summary.horizon_s = horizon_s;
    if (days.size() < 2) return summary;

    bool all_used[kEvalFeatures];
    bool imb_only[kEvalFeatures];
    for (size_t i = 0; i < kEvalFeatures; ++i) {
        all_used[i] = true;
        imb_only[i] = i == 0;
    }

    std::vector<double> ridge_means;
    std::vector<double> ridge_sample_means;
    std::vector<double> one_tick_means;
    std::vector<double> imb_means;
    std::vector<double> decile_means;
    std::vector<double> passive_means;
    std::vector<double> ablation_means[kEvalFeatures];
    double beta_sum[kEvalFeatures] = {};
    double thin_corr_sum = 0.0;

    for (size_t hold = 0; hold < days.size(); ++hold) {
        std::vector<const EvalRow*> train;
        std::vector<const EvalRow*> inner_train;
        std::vector<const EvalRow*> inner_val;
        const size_t val_day = hold == days.size() - 1 ? days.size() - 2 : days.size() - 1;
        for (size_t d = 0; d < days.size(); ++d) {
            if (d == hold) continue;
            auto p = pointers(days[d].rows);
            train.insert(train.end(), p.begin(), p.end());
            if (d == val_day) {
                inner_val.insert(inner_val.end(), p.begin(), p.end());
            } else {
                inner_train.insert(inner_train.end(), p.begin(), p.end());
            }
        }
        if (inner_train.empty()) inner_train = train;
        const Scale scale = scale_from(train);
        const double lambda = choose_lambda(inner_train, inner_val, scale, all_used);
        Fit ridge;
        if (!fit_ridge(train, scale, lambda, all_used, ridge)) continue;
        Fit imb;
        fit_ridge(train, scale, 0.0, imb_only, imb);

        double q_low = 0.0;
        double q_high = 0.0;
        const bool have_decile = train_decile_edges(train, q_low, q_high);

        RuleStats ridge_s;
        RuleStats tick_s;
        RuleStats imb_s;
        RuleStats decile_s;
        RuleStats passive_s;
        for (const EvalRow& r : days[hold].rows) {
            const double pred = predict(ridge, r);
            score_cross(ridge_s, pred, r, taker_fee);
            score_passive(passive_s, pred, r, maker_rebate);
            if (one_tick_spread(r)) score_cross(tick_s, pred, r, taker_fee);
            if (imb.ok) score_cross(imb_s, predict(imb, r), r, taker_fee);
            if (have_decile) score_decile(decile_s, r, q_low, q_high, taker_fee);
        }

        std::vector<size_t> kept;
        const uint64_t gap_ns = static_cast<uint64_t>(std::max(horizon_s, 1)) * 1'000'000'000ULL;
        thin_indices(days[hold].rows, gap_ns, kept);
        RuleStats thin_s;
        for (size_t i : kept) {
            note_pred(thin_s, predict(ridge, days[hold].rows[i]), days[hold].rows[i].target);
        }

        DayResult day;
        day.day = days[hold].label;
        day.horizon_s = horizon_s;
        day.n = ridge_s.n;
        day.corr = ridge_s.corr();
        day.corr_thinned = thin_s.corr();
        day.ridge_trades = ridge_s.trades;
        day.ridge_mean_after_cost = ridge_s.mean_after_cost();
        day.ridge_mean_per_sample = ridge_s.mean_per_sample();
        day.ridge_hit_rate = ridge_s.hit_rate();
        day.one_tick_n = tick_s.n;
        day.one_tick_trades = tick_s.trades;
        day.one_tick_mean_after_cost = tick_s.mean_after_cost();
        day.one_tick_mean_per_sample = tick_s.mean_per_sample();
        day.passive_placed = passive_s.passive_placed;
        day.passive_filled = passive_s.passive_filled;
        day.passive_fill_rate = passive_s.passive_fill_rate();
        day.passive_mean = passive_s.mean_passive();
        day.imbalance_mean_after_cost = imb_s.mean_after_cost();
        day.decile_trades = decile_s.trades;
        day.decile_mean_after_cost = decile_s.mean_after_cost();
        day.fit = ridge;
        for (size_t j = 0; j < kEvalFeatures; ++j) {
            bool dropped[kEvalFeatures];
            for (size_t i = 0; i < kEvalFeatures; ++i) dropped[i] = all_used[i];
            dropped[j] = false;
            Fit ab;
            if (!fit_ridge(train, scale, lambda, dropped, ab)) continue;
            RuleStats ab_s;
            for (const EvalRow& r : days[hold].rows) {
                score_cross(ab_s, predict(ab, r), r, taker_fee);
            }
            day.ablation_mean[j] = ab_s.mean_after_cost();
            ablation_means[j].push_back(day.ablation_mean[j]);
        }
        summary.days.push_back(day);
        ridge_means.push_back(day.ridge_mean_after_cost);
        ridge_sample_means.push_back(day.ridge_mean_per_sample);
        one_tick_means.push_back(day.one_tick_mean_after_cost);
        imb_means.push_back(day.imbalance_mean_after_cost);
        decile_means.push_back(day.decile_mean_after_cost);
        passive_means.push_back(day.passive_mean);
        thin_corr_sum += day.corr_thinned;
        if (ridge.ok) {
            for (size_t i = 0; i < kEvalFeatures; ++i) beta_sum[i] += ridge.beta[i + 1];
        }
    }

    summary.ridge = bootstrap_mean(ridge_means, seed);
    summary.ridge_per_sample = bootstrap_mean(ridge_sample_means, seed + 4);
    summary.one_tick = bootstrap_mean(one_tick_means, seed + 5);
    summary.imbalance = bootstrap_mean(imb_means, seed + 1);
    summary.decile = bootstrap_mean(decile_means, seed + 2);
    summary.passive = bootstrap_mean(passive_means, seed + 3);
    const double nday = summary.days.empty() ? 1.0 : static_cast<double>(summary.days.size());
    summary.mean_corr_thinned = thin_corr_sum / nday;
    for (size_t i = 0; i < kEvalFeatures; ++i) {
        summary.ablation[i] = bootstrap_mean(ablation_means[i], seed + 10 + i);
        summary.mean_std_beta[i] = beta_sum[i] / nday;
    }
    return summary;
}

} // namespace research
