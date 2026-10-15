#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "research/evaluate.hpp"
#include "research/run_record.hpp"

namespace {

void print_interval(const char* name, const research::Interval& iv) {
    std::cout << name << " mean_after_cost=" << iv.mean << "  95%=[" << iv.lo << ", " << iv.hi
              << "]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string ticker = "TICKER";
    std::filesystem::path out_dir = "data/eval";
    double taker_fee = 0.0;
    double maker_rebate = 0.0;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](double& dest) {
            if (i + 1 >= argc) return false;
            char* end = nullptr;
            dest = std::strtod(argv[++i], &end);
            return end != argv[i] && *end == '\0' && std::isfinite(dest);
        };
        if (arg == "--ticker" && i + 1 < argc) {
            ticker = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--taker-fee") {
            if (!need(taker_fee)) {
                std::cerr << "--taker-fee needs a finite number\n";
                return 1;
            }
        } else if (arg == "--maker-rebate") {
            if (!need(maker_rebate)) {
                std::cerr << "--maker-rebate needs a finite number\n";
                return 1;
            }
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "Unknown flag " << arg << "\n";
            return 1;
        } else {
            files.push_back(arg);
        }
    }
    if (files.size() < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " --ticker TICKER --out data/eval [--taker-fee X] [--maker-rebate Y]"
                     " <features.csv>...\n";
        std::cerr << "Leave-one-day-out ridge. It does not send orders.\n";
        return 1;
    }

    try {
        std::vector<research::EvalDay> days;
        for (const std::string& path : files) {
            research::EvalDay day;
            day.path = path;
            day.label = research::day_label_from_path(path);
            std::string err;
            // Horizon is filled per pass below; load 1s here to check the file.
            if (!research::load_features_csv(path, 0, day.rows, &err)) {
                std::cerr << err << "\n";
                return 1;
            }
            if (day.rows.empty()) {
                std::cerr << "No paired 1s rows in " << path << "\n";
                continue;
            }
            days.push_back(std::move(day));
        }
        if (days.size() < 2) {
            std::cerr << "Need at least two days with rows.\n";
            return 1;
        }

        std::filesystem::create_directories(out_dir);
        const std::filesystem::path csv_path = out_dir / (ticker + "_by_day.csv");
        std::ofstream csv(csv_path);
        if (!csv) {
            std::cerr << "Failed to open " << csv_path << "\n";
            return 1;
        }
        csv << "day,horizon_s,n,corr,corr_thinned,ridge_trades,ridge_mean_after_cost,"
               "ridge_mean_per_sample,ridge_hit_rate,one_tick_n,one_tick_trades,"
               "one_tick_mean_after_cost,one_tick_mean_per_sample,"
               "passive_placed,passive_filled,passive_fill_rate,passive_mean,"
               "imbalance_mean_after_cost,decile_trades,decile_mean_after_cost";
        for (size_t i = 0; i < research::kEvalFeatures; ++i) {
            csv << ",ablate_" << research::kEvalFeatureNames[i];
        }
        csv << '\n';
        csv << std::setprecision(10);

        std::cout << std::fixed << std::setprecision(6);
        std::cout << "Held-out-day evaluator for " << ticker << " (" << days.size()
                  << " days). Historical replay only.\n";
        std::cout << "Ridge is fit on other days. Thresholds come from training days only.\n";
        std::cout << "Crossing cost is half-spread plus taker_fee=" << taker_fee
                  << ". Passive result is the fill-model markout plus maker_rebate="
                  << maker_rebate << ".\n";
        std::cout << "Not modeled: queue position in the ridge itself, hidden liquidity, "
                     "the order's effect on others, latency, Nasdaq's fee schedule.\n";
        std::cout << "A positive mean after cost is not permission to send orders.\n\n";

        research::RunRecord record;
        record.tool = "research_evaluate";
        record.params = {{"ticker", ticker},
                         {"taker_fee", std::to_string(taker_fee)},
                         {"maker_rebate", std::to_string(maker_rebate)},
                         {"bootstrap_seed", std::to_string(research::kBootstrapSeed)},
                         {"cost_model", "cross: half_spread+taker_fee; passive: fill-model markout"},
                         {"not_modeled", "hidden liquidity, own size, latency"},
                         {"corr_thinned", "one sample per horizon"},
                         {"per_sample", "no-trade rows count as zero"}};
        record.counts.emplace_back("days", days.size());

        const int horizons[] = {1, 5, 30};
        for (int h = 0; h < 3; ++h) {
            std::vector<research::EvalDay> sliced = days;
            for (size_t d = 0; d < sliced.size(); ++d) {
                sliced[d].rows.clear();
                std::string err;
                if (!research::load_features_csv(days[d].path, h, sliced[d].rows, &err)) {
                    std::cerr << err << "\n";
                    return 1;
                }
            }
            const research::EvalSummary summary =
                research::evaluate_loo(sliced, horizons[h], taker_fee, maker_rebate);
            std::cout << "--- horizon " << horizons[h] << "s ---\n";
            for (const research::DayResult& day : summary.days) {
                csv << day.day << ',' << day.horizon_s << ',' << day.n << ',' << day.corr << ','
                    << day.corr_thinned << ',' << day.ridge_trades << ','
                    << day.ridge_mean_after_cost << ',' << day.ridge_mean_per_sample << ','
                    << day.ridge_hit_rate << ',' << day.one_tick_n << ',' << day.one_tick_trades
                    << ',' << day.one_tick_mean_after_cost << ',' << day.one_tick_mean_per_sample
                    << ',' << day.passive_placed << ',' << day.passive_filled << ','
                    << day.passive_fill_rate << ',' << day.passive_mean << ','
                    << day.imbalance_mean_after_cost << ',' << day.decile_trades << ','
                    << day.decile_mean_after_cost;
                for (size_t i = 0; i < research::kEvalFeatures; ++i) {
                    csv << ',' << day.ablation_mean[i];
                }
                csv << '\n';
                std::cout << "  " << day.day << " n=" << day.n << " corr=" << day.corr
                          << " corr_thinned=" << day.corr_thinned
                          << " ridge_trades=" << day.ridge_trades
                          << " ridge_after_cost=" << day.ridge_mean_after_cost
                          << " per_sample=" << day.ridge_mean_per_sample
                          << " one_tick_after_cost=" << day.one_tick_mean_after_cost
                          << " hit=" << day.ridge_hit_rate
                          << " passive_fill=" << day.passive_fill_rate
                          << " imb_after_cost=" << day.imbalance_mean_after_cost
                          << " decile_after_cost=" << day.decile_mean_after_cost << "\n";
            }
            print_interval("  ridge", summary.ridge);
            print_interval("  ridge per-sample", summary.ridge_per_sample);
            print_interval("  one-tick ridge", summary.one_tick);
            print_interval("  imbalance-only", summary.imbalance);
            print_interval("  train-decile", summary.decile);
            print_interval("  passive-fill", summary.passive);
            std::cout << "  mean thinned corr=" << summary.mean_corr_thinned << "\n";
            std::cout << "  mean std betas:";
            for (size_t i = 0; i < research::kEvalFeatures; ++i) {
                std::cout << " " << research::kEvalFeatureNames[i] << "="
                          << summary.mean_std_beta[i];
            }
            std::cout << "\n";
            std::cout << "  ablation mean_after_cost without:";
            for (size_t i = 0; i < research::kEvalFeatures; ++i) {
                std::cout << " " << research::kEvalFeatureNames[i] << "="
                          << summary.ablation[i].mean;
            }
            std::cout << "\n";
            std::cout << "  no-trade baseline is 0. Overlapping 100ms samples inflate corr; "
                         "corr_thinned keeps one sample per horizon. Per-sample mean counts "
                         "no-trade rows as zero. This is not a tradable edge.\n\n";
        }
        if (!csv) {
            std::cerr << "Failed while writing " << csv_path << "\n";
            return 1;
        }
        for (const auto& day : days) {
            record.inputs.push_back(research::describe_input(day.path));
        }
        record.outputs = {csv_path.string()};
        if (research::write_manifest(record, csv_path).empty()) {
            std::cerr << "Failed to write the run record for " << csv_path << "\n";
            return 1;
        }
        std::cout << "CSV: " << csv_path << "\n";
        std::cout << "Run record: " << csv_path << ".manifest.json  commit=" << QUANT_GIT_COMMIT
                  << (QUANT_GIT_DIRTY ? " (dirty)" : "") << "\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
