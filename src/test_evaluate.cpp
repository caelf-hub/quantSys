#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "research/evaluate.hpp"

using namespace research;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

EvalRow row_with(double imbalance, double target, double spread = 0.01) {
    EvalRow r;
    r.x[0] = imbalance;
    r.x[2] = spread;
    r.target = target;
    r.half_spread = spread / 2.0;
    return r;
}

std::string write_csv(const std::filesystem::path& path, const std::vector<EvalRow>& rows) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << "timestamp_ns,mid,spread,imbalance,micro_minus_mid,ofi_1s,depth5_imbalance,"
           "signed_volume_1s,mid_change_1s,minutes_since_open,target_1s,target_5s,target_30s,"
           "bid_kind,bid_queue_ahead,bid_time_to_fill_ns,bid_markout_1s,bid_markout_5s,"
           "bid_markout_30s,ask_kind,ask_queue_ahead,ask_time_to_fill_ns,ask_markout_1s,"
           "ask_markout_5s,ask_markout_30s\n";
    auto cell = [](double v) -> std::string {
        if (!std::isfinite(v)) return "";
        return std::to_string(v);
    };
    for (size_t i = 0; i < rows.size(); ++i) {
        const EvalRow& r = rows[i];
        out << (1000 * i) << ",100," << r.x[2] << ',' << r.x[0] << ",0,0,0,0,0,0," << r.target
            << ",,," << (r.bid_kind ? std::string(1, r.bid_kind) : "") << ",0,0,"
            << cell(r.bid_markout) << ",,," << (r.ask_kind ? std::string(1, r.ask_kind) : "")
            << ",0,0," << cell(r.ask_markout) << ",,\n";
    }
    return path.string();
}

} // namespace

int main() {
    // Four days, target = 2 * imbalance exactly. Other features stay 0.
    std::vector<EvalDay> days(4);
    for (int d = 0; d < 4; ++d) {
        days[static_cast<size_t>(d)].label = "day" + std::to_string(d);
        for (int i = 0; i < 20; ++i) {
            const double imb = -0.95 + 0.1 * i;
            days[static_cast<size_t>(d)].rows.push_back(row_with(imb, 2.0 * imb));
        }
    }
    const EvalSummary recovered = evaluate_loo(days, 1, /*taker_fee=*/0, /*maker_rebate=*/0);
    assert(recovered.days.size() == 4);
    for (const DayResult& day : recovered.days) {
        assert(day.fit.ok);
        assert(near(original_weight(day.fit, 0), 2.0, 1e-6));
        // Held-out predictions should match the known line.
        assert(day.corr > 0.999);
    }
    std::cout << "[PASS] leave-one-day-out recovers the known imbalance coefficient\n";

    // Day 3 has a 100x coefficient. Holding it out, the fit must stay at 2,
    // not 100: the held-out day never enters its own fit.
    std::vector<EvalDay> spiked = days;
    for (EvalRow& r : spiked[3].rows) r.target = 100.0 * r.x[0];
    const EvalSummary isolated = evaluate_loo(spiked, 1, 0, 0);
    double held3 = 0.0;
    double held0 = 0.0;
    for (const DayResult& day : isolated.days) {
        if (day.day == "day3") held3 = original_weight(day.fit, 0);
        if (day.day == "day0") held0 = original_weight(day.fit, 0);
    }
    assert(near(held3, 2.0, 1e-4));
    assert(held0 > 10.0);  // training on the spike pulls the slope up
    std::cout << "[PASS] the held-out day never enters its own fit\n";

    // Train imbalances are all negative; the test day is all positive.
    // Train-only outer deciles therefore never sell on the test day.
    // The same day's own deciles (look-ahead) would sell the lowest tenth.
    std::vector<EvalDay> split(2);
    split[0].label = "train";
    split[1].label = "test";
    for (int i = 0; i < 30; ++i) {
        split[0].rows.push_back(row_with(-1.0 + 0.01 * i, 0.0));
        split[1].rows.push_back(row_with(0.5 + 0.01 * i, 0.02));
    }
    const EvalSummary dec = evaluate_loo(split, 1, 0, 0);
    DayResult test_day;
    for (const DayResult& day : dec.days) {
        if (day.day == "test") test_day = day;
    }
    assert(test_day.decile_trades == 30);  // every test row is above train q_high
    auto train_p = pointers(split[0].rows);
    auto test_p = pointers(split[1].rows);
    double q_low = 0, q_high = 0;
    assert(train_decile_edges(train_p, q_low, q_high));
    double look_low = 0, look_high = 0;
    assert(train_decile_edges(test_p, look_low, look_high));
    uint64_t lookahead_sells = 0;
    for (const EvalRow& r : split[1].rows) {
        if (r.x[0] <= look_low) ++lookahead_sells;
    }
    uint64_t train_only_sells = 0;
    for (const EvalRow& r : split[1].rows) {
        if (r.x[0] <= q_low) ++train_only_sells;
    }
    assert(lookahead_sells > 0);
    assert(train_only_sells == 0);
    std::cout << "[PASS] decile thresholds come from training days only\n";

    const Interval a = bootstrap_mean({1.0, 2.0, 3.0, 4.0, 5.0}, 1);
    const Interval b = bootstrap_mean({1.0, 2.0, 3.0, 4.0, 5.0}, 1);
    const Interval other = bootstrap_mean({10.0, 20.0, 30.0, 40.0, 50.0}, 1);
    assert(near(a.mean, 3.0));
    assert(near(a.lo, b.lo) && near(a.hi, b.hi));
    assert(!near(a.mean, other.mean));
    std::cout << "[PASS] bootstrap interval is reproducible from its seed\n";

    const auto dir = std::filesystem::temp_directory_path() / "quant_test_evaluate";
    std::vector<EvalRow> csv_rows = {row_with(-0.5, -1.0), row_with(0.5, 1.0)};
    csv_rows[0].bid_kind = 'Q';
    csv_rows[0].bid_markout = 0.02;
    const auto csv_path = dir / "12302019" / "AAPL_features.csv";
    write_csv(csv_path, csv_rows);
    std::vector<EvalRow> loaded;
    std::string err;
    assert(load_features_csv(csv_path.string(), 0, loaded, &err));
    assert(loaded.size() == 2);
    assert(near(loaded[0].x[0], -0.5));
    assert(near(loaded[0].target, -1.0));
    assert(loaded[0].bid_kind == 'Q');
    assert(near(loaded[0].bid_markout, 0.02));
    assert(std::isnan(loaded[0].ask_markout) || loaded[0].ask_kind == 0);
    assert(day_label_from_path(csv_path.string()) == "12302019");
    assert(loaded[0].timestamp_ns == 0);
    assert(loaded[1].timestamp_ns == 1000);
    std::filesystem::remove_all(dir);
    std::cout << "[PASS] features CSV loader keeps empty cells as missing\n";

    // Two trades of +0.01 among ten rows: per-trade mean is 0.01, per-sample
    // mean includes the eight zeros.
    RuleStats sparse;
    for (int i = 0; i < 10; ++i) {
        EvalRow r = row_with(0.0, 0.02, 0.01);
        if (i < 2) note_trade(sparse, 0.01);
        ++sparse.n;
        (void)r;
    }
    assert(near(sparse.mean_after_cost(), 0.01));
    assert(near(sparse.mean_per_sample(), 0.002));
    std::cout << "[PASS] per-sample after-cost includes rows with no trade\n";

    // 100 ms samples. Thinning to 1s keeps every tenth timestamp.
    std::vector<EvalRow> dense(20);
    for (int i = 0; i < 20; ++i) {
        dense[static_cast<size_t>(i)].timestamp_ns = static_cast<uint64_t>(i) * 100'000'000ULL;
        dense[static_cast<size_t>(i)].target = static_cast<double>(i);
        dense[static_cast<size_t>(i)].x[0] = static_cast<double>(i);
    }
    std::vector<size_t> kept;
    assert(thin_indices(dense, 1'000'000'000ULL, kept) == 2);
    assert(kept[0] == 0 && kept[1] == 10);
    std::cout << "[PASS] thinning keeps samples at least one horizon apart\n";

    // One-tick scoring ignores the wide-spread row.
    std::vector<EvalDay> mixed(2);
    mixed[0].label = "a";
    mixed[1].label = "b";
    for (int i = 0; i < 20; ++i) {
        mixed[0].rows.push_back(row_with(-0.9 + 0.09 * i, 0.02 * (-0.9 + 0.09 * i), 0.01));
        mixed[1].rows.push_back(row_with(-0.9 + 0.09 * i, 0.02 * (-0.9 + 0.09 * i), 0.01));
    }
    mixed[1].rows[0].x[2] = 0.10;
    mixed[1].rows[0].half_spread = 0.05;
    mixed[1].rows[0].target = 1.0;
    const EvalSummary tick = evaluate_loo(mixed, 1, 0, 0);
    DayResult held_b;
    for (const DayResult& day : tick.days) {
        if (day.day == "b") held_b = day;
    }
    assert(held_b.one_tick_n == mixed[1].rows.size() - 1);
    assert(held_b.n == mixed[1].rows.size());
    std::cout << "[PASS] one-tick slice drops wide-spread rows\n";

    // Signal is imbalance. Ablating it must change held-out after-cost
    // relative to dropping a constant (zero-variance) column.
    const EvalSummary ablated = evaluate_loo(mixed, 1, 0, 0);
    double without_imb = 0.0;
    double without_ofi = 0.0;
    for (const DayResult& day : ablated.days) {
        without_imb += day.ablation_mean[0];
        without_ofi += day.ablation_mean[3];
    }
    assert(!near(without_imb, without_ofi, 1e-9) || ablated.days.empty());
    assert(ablated.days.size() == 2);
    std::cout << "[PASS] ablation reports a different mean when imbalance is dropped\n";

    std::cout << "All evaluator tests passed.\n";
    return 0;
}
