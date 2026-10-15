#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/order_book.hpp"
#include "itch/mmap_reader.hpp"
#include "research/features.hpp"
#include "research/fill_model.hpp"
#include "research/imbalance_study.hpp"
#include "research/run_record.hpp"

using namespace itch;
using namespace book;

namespace {

constexpr uint64_t kSampleIntervalNs = 100'000'000ULL; // one book snapshot per 100ms

std::string trim_symbol(const std::array<char, 8>& raw) {
    std::string symbol(raw.data(), raw.size());
    while (!symbol.empty() && symbol.back() == ' ') symbol.pop_back();
    return symbol;
}

bool ticker_is_safe(const std::string& ticker) {
    if (ticker.empty() || ticker.size() > 8) return false;
    return ticker.find('/') == std::string::npos && ticker.find('\\') == std::string::npos;
}

bool parse_fee(const char* text, double& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || std::fabs(value) > 1.0) {
        return false;
    }
    out = value;
    return true;
}

// Empty for NaN so the CSV reader sees a missing value.
void put(std::ostream& out, double value) {
    if (!std::isnan(value)) out << value;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <path-to-itch-file> <ticker> [csv-path] [--maker-rebate X]"
                     " [--taker-fee Y]\n";
        std::cerr << "Writes a hypothesis report. It does not send orders.\n";
        std::cerr << "Fees are per share, default 0, and are not Nasdaq's schedule.\n";
        return 1;
    }

    const std::string path = argv[1];
    const std::string ticker = argv[2];
    if (!ticker_is_safe(ticker)) {
        std::cerr << "Ticker must be 1-8 characters and must not contain a path separator.\n";
        return 1;
    }
    std::filesystem::path csv_path = std::filesystem::path("data") / (ticker + "_imbalance.csv");
    research::FillParams fill_params;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--maker-rebate" || arg == "--taker-fee") {
            double value = 0.0;
            if (i + 1 >= argc || !parse_fee(argv[i + 1], value)) {
                std::cerr << arg << " needs a finite per-share value between -1 and 1.\n";
                return 1;
            }
            (arg == "--maker-rebate" ? fill_params.maker_rebate : fill_params.taker_fee) = value;
            ++i;
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "Unknown flag " << arg << "\n";
            return 1;
        } else {
            csv_path = arg;
        }
    }

    try {
        MmapReader reader(path);

        std::unordered_map<uint16_t, std::string> locate_to_ticker;
        ParserCallbacks directory_cb;
        directory_cb.on_stock_directory = [&](const StockDirectory& message) {
            locate_to_ticker[message.header.stock_locate] = trim_symbol(message.stock);
        };
        reader.process_all(directory_cb);

        uint16_t target_locate = 0;
        bool found = false;
        for (const auto& [locate, name] : locate_to_ticker) {
            if (name == ticker) {
                target_locate = locate;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Ticker '" << ticker << "' not found in this file.\n";
            return 1;
        }

        // One symbol, large pool. The full-market replay caps every name at
        // 5,000 orders and would truncate a liquid book before this study.
        OrderBook book(/*min=*/1 * 10000, /*max=*/2000 * 10000, /*tick=*/100,
                        /*max_orders=*/2'000'000);
        std::vector<research::BookSample> samples;
        std::vector<research::FeatureRow> rows;
        // Index into sim.orders() for each sample's hypothetical bid and ask.
        std::vector<int64_t> bid_order;
        std::vector<int64_t> ask_order;
        research::FillSimulator sim(fill_params, /*reserve_refs=*/2'000'000);
        research::FeatureTracker tracker;
        uint64_t last_ts = 0;

        auto consider = [&](uint64_t timestamp_ns) {
            const double bid = book.best_bid();
            const double ask = book.best_ask();
            if (bid < 0.0 || ask < 0.0) return;
            const uint64_t bid_shares = book.best_bid_shares();
            const uint64_t ask_shares = book.best_ask_shares();
            if (bid_shares == 0 || ask_shares == 0) return;
            if (!samples.empty()) {
                const uint64_t previous = samples.back().timestamp_ns;
                if (timestamp_ns < previous) return;
                if (timestamp_ns < previous + kSampleIntervalNs) return;
            }
            const double spread = ask - bid;
            research::BookSample sample;
            sample.timestamp_ns = timestamp_ns;
            sample.mid = (bid + ask) / 2.0;
            sample.spread = spread;
            sample.imbalance = research::top_imbalance(bid_shares, ask_shares);
            sample.microprice = research::top_microprice(bid, ask, bid_shares, ask_shares);
            sample.half_spread = std::max(spread, 0.0) / 2.0;
            sample.bid = bid;
            sample.ask = ask;
            sample.bid_gap_ticks = book.next_bid_gap_ticks();
            sample.ask_gap_ticks = book.next_ask_gap_ticks();
            sample.inside_touch = book.touch_has_inside_level();
            samples.push_back(sample);

            research::FeatureRow row;
            row.timestamp_ns = timestamp_ns;
            row.mid = sample.mid;
            row.spread = spread;
            row.imbalance = sample.imbalance;
            row.micro_minus_mid = sample.microprice - sample.mid;
            row.ofi_1s = static_cast<double>(tracker.ofi_1s(timestamp_ns));
            row.depth5_imbalance =
                research::depth_imbalance(book.bid_depth_within(research::kDepthTicks),
                                          book.ask_depth_within(research::kDepthTicks));
            row.signed_volume_1s = static_cast<double>(tracker.signed_volume_1s(timestamp_ns));
            row.minutes_since_open =
                research::minutes_since(timestamp_ns, research::kRegularOpenNs);
            rows.push_back(row);
            rows.back().mid_change_1s =
                research::lagged_mid_change(rows, rows.size() - 1, research::kFeatureWindowNs);

            const uint32_t bid_raw = book.best_bid_raw();
            const uint32_t ask_raw = book.best_ask_raw();
            const bool eligible = research::in_regular_hours(timestamp_ns) &&
                                  ask_raw == bid_raw + fill_params.tick_raw;
            const size_t index = samples.size() - 1;
            bid_order.push_back(eligible ? static_cast<int64_t>(sim.place(
                                               index, timestamp_ns, 'B', bid_raw, bid_shares))
                                         : -1);
            ask_order.push_back(eligible ? static_cast<int64_t>(sim.place(
                                               index, timestamp_ns, 'S', ask_raw, ask_shares))
                                         : -1);
        };

        // After every event for the symbol: the touch moves the fill model
        // and OFI, then the sampler decides whether to snapshot.
        auto after_event = [&](uint64_t ts) {
            last_ts = ts;
            research::TouchState touch;
            touch.bid_raw = book.best_bid_raw();
            touch.ask_raw = book.best_ask_raw();
            touch.bid_shares = book.best_bid_shares();
            touch.ask_shares = book.best_ask_shares();
            sim.on_touch(ts, touch.bid_raw, touch.ask_raw);
            tracker.on_touch(ts, touch);
            consider(ts);
        };

        ParserCallbacks callbacks;
        callbacks.on_add_order = [&](const AddOrder& message) {
            if (message.header.stock_locate != target_locate) return;
            book.add_order(message.order_ref_number, message.buy_sell_indicator,
                           message.price_raw, message.shares);
            sim.on_add(message.order_ref_number, message.buy_sell_indicator, message.price_raw,
                       message.shares);
            after_event(message.header.timestamp_ns);
        };
        callbacks.on_add_order_mpid = [&](const AddOrderMPID& message) {
            if (message.header.stock_locate != target_locate) return;
            book.add_order(message.order_ref_number, message.buy_sell_indicator,
                           message.price_raw, message.shares);
            sim.on_add(message.order_ref_number, message.buy_sell_indicator, message.price_raw,
                       message.shares);
            after_event(message.header.timestamp_ns);
        };
        callbacks.on_order_cancel = [&](const OrderCancel& message) {
            if (message.header.stock_locate != target_locate) return;
            book.cancel_shares(message.order_ref_number, message.cancelled_shares);
            sim.on_cancel(message.header.timestamp_ns, message.order_ref_number,
                          message.cancelled_shares);
            after_event(message.header.timestamp_ns);
        };
        callbacks.on_order_delete = [&](const OrderDelete& message) {
            if (message.header.stock_locate != target_locate) return;
            book.delete_order(message.order_ref_number);
            sim.on_delete(message.header.timestamp_ns, message.order_ref_number);
            after_event(message.header.timestamp_ns);
        };
        auto on_execution = [&](uint64_t ts, uint64_t ref, uint32_t shares) {
            book.execute_shares(ref, shares);
            tracker.on_execution(ts, sim.side_of(ref), shares);
            sim.on_execute(ts, ref, shares);
            after_event(ts);
        };
        callbacks.on_order_executed = [&](const OrderExecuted& message) {
            if (message.header.stock_locate != target_locate) return;
            on_execution(message.header.timestamp_ns, message.order_ref_number,
                         message.executed_shares);
        };
        callbacks.on_order_executed_with_price = [&](const OrderExecutedWithPrice& message) {
            if (message.header.stock_locate != target_locate) return;
            on_execution(message.header.timestamp_ns, message.order_ref_number,
                         message.executed_shares);
        };
        callbacks.on_order_replace = [&](const OrderReplace& message) {
            if (message.header.stock_locate != target_locate) return;
            book.replace_order(message.original_order_ref_number, message.new_order_ref_number,
                               message.price_raw, message.shares);
            sim.on_replace(message.header.timestamp_ns, message.original_order_ref_number,
                           message.new_order_ref_number, message.price_raw, message.shares);
            after_event(message.header.timestamp_ns);
        };
        // Hidden-liquidity prints feed signed volume only. They do not touch
        // the displayed queue, so they never fill a hypothetical order.
        callbacks.on_trade = [&](const Trade& message) {
            if (message.header.stock_locate != target_locate) return;
            tracker.on_execution(message.header.timestamp_ns, message.buy_sell_indicator,
                                 message.shares);
        };

        const size_t total_messages = reader.process_all(callbacks);
        research::attach_forward_mids(samples, research::kForwardHorizonNs);
        sim.finish(last_ts);
        {
            std::vector<uint64_t> mid_ts;
            std::vector<double> mids;
            mid_ts.reserve(samples.size());
            mids.reserve(samples.size());
            for (const research::BookSample& s : samples) {
                mid_ts.push_back(s.timestamp_ns);
                mids.push_back(s.mid);
            }
            research::attach_markouts(sim.orders(), mid_ts, mids);
        }
        research::attach_targets(rows);

        std::filesystem::create_directories(csv_path.parent_path().empty()
                                                ? std::filesystem::path(".")
                                                : csv_path.parent_path());
        std::ofstream out(csv_path);
        if (!out) {
            std::cerr << "Failed to open " << csv_path << " for writing.\n";
            return 1;
        }
        out << "timestamp_ns,mid,spread,imbalance,microprice,forward_mid,mid_change,"
               "half_spread,change_after_cost\n";
        out << std::setprecision(10);
        std::size_t paired = 0;
        for (const research::BookSample& sample : samples) {
            if (!research::has_forward(sample)) continue;
            const double change = sample.forward_mid - sample.mid;
            out << sample.timestamp_ns << ',' << sample.mid << ',' << sample.spread << ','
                << sample.imbalance << ',' << sample.microprice << ',' << sample.forward_mid
                << ',' << change << ',' << sample.half_spread << ','
                << (change - sample.half_spread) << '\n';
            ++paired;
        }
        if (!out) {
            std::cerr << "Failed while writing " << csv_path << ".\n";
            return 1;
        }

        const std::filesystem::path decisions_path =
            csv_path.parent_path() / (ticker + "_decisions.csv");
        const DropCounts drops = book.drop_counts();
        const auto decisions = research::hypothetical_decisions(samples);
        std::ofstream decisions_out(decisions_path);
        if (!decisions_out) {
            std::cerr << "Failed to open " << decisions_path << " for writing.\n";
            return 1;
        }
        decisions_out << "timestamp_ns,side,bid,ask,spread,imbalance,forward_mid,half_spread,"
                         "price_out_of_band,pool_exhausted,unknown_order,duplicate_order\n";
        decisions_out << std::setprecision(10);
        for (const research::DecisionRow& row : decisions) {
            decisions_out << row.timestamp_ns << ',' << row.side << ',' << row.bid << ','
                          << row.ask << ',' << row.spread << ',' << row.imbalance << ','
                          << row.forward_mid << ',' << row.half_spread << ','
                          << drops.price_out_of_band << ',' << drops.pool_exhausted << ','
                          << drops.unknown_order << ',' << drops.duplicate_order << '\n';
        }
        if (!decisions_out) {
            std::cerr << "Failed while writing " << decisions_path << ".\n";
            return 1;
        }

        const std::filesystem::path features_path =
            csv_path.parent_path() / (ticker + "_features.csv");
        std::ofstream features_out(features_path);
        if (!features_out) {
            std::cerr << "Failed to open " << features_path << " for writing.\n";
            return 1;
        }
        features_out << "timestamp_ns,mid,spread,imbalance,micro_minus_mid,ofi_1s,"
                        "depth5_imbalance,signed_volume_1s,mid_change_1s,minutes_since_open,"
                        "target_1s,target_5s,target_30s";
        for (const char* side : {"bid", "ask"}) {
            features_out << ',' << side << "_kind," << side << "_queue_ahead," << side
                         << "_time_to_fill_ns," << side << "_markout_1s," << side
                         << "_markout_5s," << side << "_markout_30s";
        }
        features_out << '\n' << std::setprecision(10);
        for (size_t i = 0; i < rows.size(); ++i) {
            const research::FeatureRow& r = rows[i];
            features_out << r.timestamp_ns << ',' << r.mid << ',' << r.spread << ','
                         << r.imbalance << ',' << r.micro_minus_mid << ',' << r.ofi_1s << ','
                         << r.depth5_imbalance << ',' << r.signed_volume_1s << ','
                         << r.mid_change_1s << ',' << r.minutes_since_open;
            for (int h = 0; h < 3; ++h) {
                features_out << ',';
                put(features_out, r.target[h]);
            }
            for (const int64_t idx : {bid_order[i], ask_order[i]}) {
                if (idx < 0) {
                    features_out << ",,,,,,";
                    continue;
                }
                const research::PassiveOrder& o = sim.orders()[static_cast<size_t>(idx)];
                features_out << ',' << static_cast<char>(o.kind) << ',' << o.queue_ahead_at_entry
                             << ',' << o.time_to_fill_ns();
                for (int h = 0; h < 3; ++h) {
                    features_out << ',';
                    put(features_out, o.markout[h]);
                }
            }
            features_out << '\n';
        }
        if (!features_out) {
            std::cerr << "Failed while writing " << features_path << ".\n";
            return 1;
        }

        uint64_t placed[2] = {0, 0};
        uint64_t filled[2] = {0, 0};
        uint64_t trade_through[2] = {0, 0};
        double markout_sum[2][3] = {{0, 0, 0}, {0, 0, 0}};
        uint64_t markout_n[2][3] = {{0, 0, 0}, {0, 0, 0}};
        for (const research::PassiveOrder& o : sim.orders()) {
            const int s = o.side == 'B' ? 0 : 1;
            ++placed[s];
            if (!research::is_fill(o.kind)) continue;
            ++filled[s];
            if (o.kind == research::FillKind::TradeThrough) ++trade_through[s];
            for (int h = 0; h < 3; ++h) {
                if (std::isnan(o.markout[h])) continue;
                markout_sum[s][h] += o.markout[h];
                ++markout_n[s][h];
            }
        }

        const auto deciles = research::imbalance_deciles(samples);
        const research::TouchSummary touch = research::summarize_regular_touch(samples);

        research::InputFile tape;
        tape.ok = true;
        tape.path = path;
        tape.bytes = reader.size();
        tape.fnv1a64 = research::fnv1a64(reader.data(), reader.size());
        research::RunRecord record;
        record.tool = "research_imbalance";
        record.inputs.push_back(tape);
        record.params = {{"ticker", ticker},
                         {"stock_locate", std::to_string(target_locate)},
                         {"sample_interval_ns", std::to_string(kSampleIntervalNs)},
                         {"forward_horizon_ns", std::to_string(research::kForwardHorizonNs)},
                         {"ladder", "1-2000 USD tick 0.01"},
                         {"max_orders", "2000000"},
                         {"cost_model", "half_spread lower bound; fill model in features CSV"},
                         {"fill_order_shares", std::to_string(fill_params.order_shares)},
                         {"fill_max_life_ns", std::to_string(fill_params.max_life_ns)},
                         {"fill_max_move_ticks", std::to_string(fill_params.max_move_ticks)},
                         {"maker_rebate", std::to_string(fill_params.maker_rebate)},
                         {"taker_fee", std::to_string(fill_params.taker_fee)},
                         {"not_modeled",
                          "hidden liquidity, own order's effect on others, latency"}};
        record.counts = {{"messages", total_messages},
                         {"short_frames", reader.frame_stats().short_frames},
                         {"truncated_tail", reader.frame_stats().truncated_tail},
                         {"samples", samples.size()},
                         {"paired", paired},
                         {"decisions", decisions.size()},
                         {"price_out_of_band", drops.price_out_of_band},
                         {"pool_exhausted", drops.pool_exhausted},
                         {"unknown_order", drops.unknown_order},
                         {"duplicate_order", drops.duplicate_order},
                         {"feature_rows", rows.size()},
                         {"passive_bids", placed[0]},
                         {"passive_bids_filled", filled[0]},
                         {"passive_asks", placed[1]},
                         {"passive_asks_filled", filled[1]}};
        record.outputs = {csv_path.string(), decisions_path.string(), features_path.string()};
        for (const auto& output : {csv_path, decisions_path, features_path}) {
            if (research::write_manifest(record, output).empty()) {
                std::cerr << "Failed to write the run record for " << output << ".\n";
                return 1;
            }
        }

        std::cout << std::fixed << std::setprecision(6);
        std::cout << "Hypothesis report for " << ticker << " (historical replay only).\n";
        std::cout << "Samples are one two-sided book per 100ms of ITCH time.\n";
        std::cout << "Forward mid is the first sample at least 1s later.\n";
        std::cout << "Half-spread is max(spread, 0) / 2. That is a lower bound on cost, "
                     "not a fill model.\n";
        std::cout << "Queue position and adverse selection are not modeled.\n";
        std::cout << "A positive decile is not permission to send orders.\n";
        std::cout << "Hypothetical decisions are regular-hours, one-cent spreads in the "
                     "outer imbalance deciles.\n";
        std::cout << "A decision row is not a fill. Joining the queue is not modeled.\n\n";
        std::cout << "Messages in file: " << total_messages << "\n";
        std::cout << "Frames: short=" << reader.frame_stats().short_frames
                  << " truncated_tail=" << reader.frame_stats().truncated_tail << "\n";
        std::cout << "Samples: " << samples.size() << "  paired with a forward mid: " << paired
                  << "\n";
        std::cout << "Drops: price_out_of_band=" << drops.price_out_of_band
                  << " pool_exhausted=" << drops.pool_exhausted
                  << " unknown_order=" << drops.unknown_order
                  << " duplicate_order=" << drops.duplicate_order << "\n";
        std::cout << "Regular hours (09:30-16:00): " << touch.regular_hours_count << "\n";
        std::cout << "median_spread=" << touch.median_spread
                  << "  one_tick_fraction=" << touch.one_tick_fraction
                  << "  one_tick_behind_fraction=" << touch.one_tick_behind_fraction
                  << "  inside_touch=" << touch.inside_touch_count << "\n";
        std::cout << "CSV: " << csv_path << "\n";
        std::cout << "Decisions CSV: " << decisions_path << "  rows=" << decisions.size()
                  << "\n";
        std::cout << "Features CSV: " << features_path << "  rows=" << rows.size() << "\n";
        std::cout << "Run record: <csv>.manifest.json  commit=" << QUANT_GIT_COMMIT
                  << (QUANT_GIT_DIRTY ? " (dirty)" : "") << "  fnv1a64=" << research::hex64(tape.fnv1a64)
                  << "\n\n";

        if (deciles.empty()) {
            std::cout << "No paired samples. No decile table.\n";
        } else {
            std::cout << "Deciles by top-of-book imbalance "
                         "(0 = most ask-heavy, 9 = most bid-heavy)\n";
            for (const research::DecileRow& row : deciles) {
                std::cout << "  bucket " << row.bucket << "  n=" << row.count
                          << "  mean_change=" << row.mean_change
                          << "  after_half_spread=" << row.mean_change_after_cost << "\n";
            }
        }

        const research::WalkForwardResult baseline = research::walk_forward_baseline(samples);
        std::cout << "\nWalk-forward baseline: least squares of the 1s mid change on "
                     "imbalance and spread.\n";
        std::cout << "Fit on the first half of regular-hours samples. The second half is "
                     "held out.\n";
        std::cout << "Half-spread is subtracted after the fit. A positive coefficient is "
                     "not permission to send orders.\n";
        if (!baseline.ok) {
            std::cout << "No fit.\n";
        } else {
            std::cout << "train=" << baseline.train_count << "  test=" << baseline.test_count
                      << "\n";
            std::cout << "intercept=" << baseline.intercept
                      << "  imbalance_slope=" << baseline.imbalance_slope
                      << "  spread_slope=" << baseline.spread_slope << "\n";
            std::cout << "in_sample_mean_change=" << baseline.in_sample_mean_change
                      << "  after_half_spread=" << baseline.in_sample_mean_after_cost << "\n";
            std::cout << "out_of_sample_mean_change=" << baseline.out_of_sample_mean_change
                      << "  after_half_spread=" << baseline.out_of_sample_mean_after_cost
                      << "\n";
            std::cout << "Drops: price_out_of_band=" << drops.price_out_of_band
                      << " pool_exhausted=" << drops.pool_exhausted
                      << " unknown_order=" << drops.unknown_order
                      << " duplicate_order=" << drops.duplicate_order << "\n";
        }

        std::cout << "\nQueue-position fill model: one hypothetical " << fill_params.order_shares
                  << "-share bid and ask at the touch per regular-hours one-cent sample.\n";
        std::cout << "Fills when executions on later orders at that price reach the order "
                     "size, or the opposite touch reaches it. Pulled after 10s or a move of "
                     "more than one tick.\n";
        std::cout << "Not modeled: hidden liquidity (P trades), the order's own effect on "
                     "others, latency. Fees: maker_rebate="
                  << fill_params.maker_rebate << " taker_fee=" << fill_params.taker_fee
                  << " per share (parameters, not Nasdaq's schedule).\n";
        for (int s = 0; s < 2; ++s) {
            std::cout << (s == 0 ? "  bid" : "  ask") << ": placed=" << placed[s]
                      << " filled=" << filled[s] << " trade_through=" << trade_through[s]
                      << " fill_rate="
                      << (placed[s] ? static_cast<double>(filled[s]) / placed[s] : 0.0);
            for (int h = 0; h < 3; ++h) {
                std::cout << " markout_" << (h == 0 ? "1s" : h == 1 ? "5s" : "30s") << "="
                          << (markout_n[s][h] ? markout_sum[s][h] / markout_n[s][h] : 0.0);
            }
            std::cout << "\n";
        }
        std::cout << "Markouts are per share before the rebate. A positive markout is not "
                     "permission to send orders.\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
