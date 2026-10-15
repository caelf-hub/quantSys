#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "research/imbalance_study.hpp"

using research::BookSample;
using research::attach_forward_mids;
using research::imbalance_deciles;
using research::kForwardHorizonNs;
using research::top_imbalance;
using research::top_microprice;

static bool approx_eq(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

int main() {
    assert(approx_eq(top_imbalance(300, 100), 0.5));
    assert(approx_eq(top_imbalance(0, 0), 0.0));
    assert(approx_eq(top_microprice(10.0, 11.0, 300, 100), (11.0 * 300.0 + 10.0 * 100.0) / 400.0));
    std::cout << "[PASS] imbalance and microprice formulas\n";

    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<BookSample> samples = {
        BookSample{0, 10.0, 0.02, 0.0, 10.0, 0.01, nan},
        BookSample{500'000'000ULL, 11.0, 0.02, 0.0, 11.0, 0.01, nan},
        BookSample{1'000'000'000ULL, 12.0, 0.02, 0.0, 12.0, 0.01, nan},
        BookSample{1'500'000'000ULL, 13.0, 0.02, 0.0, 13.0, 0.01, nan},
    };
    attach_forward_mids(samples, kForwardHorizonNs);
    assert(approx_eq(samples[0].forward_mid, 12.0));
    assert(approx_eq(samples[1].forward_mid, 13.0));
    assert(!research::has_forward(samples[2]));
    assert(!research::has_forward(samples[3]));
    std::cout << "[PASS] forward mid is the first sample at least one second later\n";

    std::vector<BookSample> decile_input;
    for (int i = 0; i < 10; ++i) {
        const double imbalance = static_cast<double>(i) - 5.0; // -5 .. 4
        BookSample sample;
        sample.imbalance = imbalance;
        sample.mid = 100.0;
        sample.forward_mid = 100.0 + imbalance;
        sample.half_spread = 1.0;
        decile_input.push_back(sample);
    }
    const auto rows = imbalance_deciles(decile_input);
    assert(rows.size() == 10);
    assert(rows.front().count == 1);
    assert(approx_eq(rows.front().mean_change, -5.0));
    assert(approx_eq(rows.front().mean_change_after_cost, -6.0));
    assert(approx_eq(rows.back().mean_change, 4.0));
    assert(approx_eq(rows.back().mean_change_after_cost, 3.0));
    assert(imbalance_deciles({}).empty());
    std::cout << "[PASS] imbalance deciles sort ask-heavy to bid-heavy and subtract half-spread\n";

    const uint64_t rth = research::kRegularOpenNs + 60'000'000'000ULL; // 09:31
    const uint64_t premarket = research::kRegularOpenNs - 1;
    BookSample penny;
    penny.timestamp_ns = rth;
    penny.spread = 0.01;
    penny.bid = 291.20;
    penny.ask = 291.21;
    penny.imbalance = -5.0;
    penny.mid = 291.205;
    penny.forward_mid = 291.20;
    penny.half_spread = 0.005;
    penny.bid_gap_ticks = 1;
    penny.ask_gap_ticks = 1;
    BookSample three_cents = penny;
    three_cents.timestamp_ns = rth + 100'000'000ULL;
    three_cents.spread = 0.03;
    three_cents.ask = 291.23;
    three_cents.bid_gap_ticks = 3;
    three_cents.ask_gap_ticks = 1;
    three_cents.imbalance = 4.0;
    BookSample before_open = penny;
    before_open.timestamp_ns = premarket;
    before_open.inside_touch = true;

    const research::TouchSummary summary =
        research::summarize_regular_touch({penny, three_cents, before_open});
    assert(summary.regular_hours_count == 2);
    assert(approx_eq(summary.median_spread, 0.02));
    assert(approx_eq(summary.one_tick_fraction, 0.5));
    assert(approx_eq(summary.one_tick_behind_fraction, 0.5));
    assert(summary.inside_touch_count == 0);
    assert(research::summarize_regular_touch({}).regular_hours_count == 0);
    std::cout << "[PASS] regular-hours touch summary ignores the open and counts one-tick gaps\n";

    std::vector<BookSample> decisions_input;
    for (int i = 0; i < 10; ++i) {
        BookSample sample = penny;
        sample.timestamp_ns = rth + static_cast<uint64_t>(i) * 100'000'000ULL;
        sample.imbalance = static_cast<double>(i) - 5.0; // -5 .. 4
        sample.forward_mid = sample.mid;
        decisions_input.push_back(sample);
    }
    BookSample wide = decisions_input.front();
    wide.spread = 0.03;
    wide.imbalance = -100.0;
    wide.timestamp_ns = rth + 2'000'000'000ULL;
    decisions_input.push_back(wide);
    BookSample early = decisions_input.back();
    early.timestamp_ns = premarket;
    early.spread = 0.01;
    early.imbalance = 100.0;
    decisions_input.push_back(early);
    BookSample unpaired = penny;
    unpaired.timestamp_ns = rth + 3'000'000'000ULL;
    unpaired.imbalance = 50.0;
    unpaired.forward_mid = nan;
    decisions_input.push_back(unpaired);

    const auto decisions = research::hypothetical_decisions(decisions_input);
    assert(decisions.size() == 2);
    assert(decisions[0].side == 'S');
    assert(approx_eq(decisions[0].imbalance, -5.0));
    assert(approx_eq(decisions[0].bid, 291.20));
    assert(approx_eq(decisions[0].ask, 291.21));
    assert(decisions[1].side == 'B');
    assert(approx_eq(decisions[1].imbalance, 4.0));
    assert(research::hypothetical_decisions({}).empty());
    std::cout << "[PASS] hypothetical decisions are the outer one-tick regular-hours deciles\n";

    const uint64_t noon = research::kRegularOpenNs + 3'600'000'000'000ULL;
    std::vector<BookSample> line;
    for (int i = 0; i < 8; ++i) {
        BookSample sample;
        sample.timestamp_ns = noon + static_cast<uint64_t>(i) * 100'000'000ULL;
        sample.imbalance = static_cast<double>(i);
        sample.spread = (i % 2 == 0) ? 0.01 : 0.03;
        sample.half_spread = sample.spread / 2.0;
        sample.mid = 100.0;
        if (i < 4) {
            sample.forward_mid = sample.mid + (1.0 + 2.0 * sample.imbalance + 3.0 * sample.spread);
        } else {
            sample.forward_mid = sample.mid + 100.0;
        }
        line.push_back(sample);
    }
    const research::WalkForwardResult fit = research::walk_forward_baseline(line);
    assert(fit.ok);
    assert(fit.train_count == 4);
    assert(fit.test_count == 4);
    assert(approx_eq(fit.intercept, 1.0, 1e-6));
    assert(approx_eq(fit.imbalance_slope, 2.0, 1e-6));
    assert(approx_eq(fit.spread_slope, 3.0, 1e-6));
    assert(approx_eq(fit.out_of_sample_mean_change, 100.0, 1e-6));
    assert(!research::walk_forward_baseline({}).ok);
    std::cout << "[PASS] walk-forward fit uses only the first half and recovers the line\n";

    std::cout << "All imbalance study tests passed.\n";
    return 0;
}
