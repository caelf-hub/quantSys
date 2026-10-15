#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/capacity_plan.hpp"
#include "book/locate_indexed_book.hpp"
#include "book/sharded_replay.hpp"
#include "itch/mmap_reader.hpp"
#include "itch/parser.hpp"

using namespace itch;
using namespace book;

static std::string trim(const std::array<char, 8>& raw) {
    std::string s(raw.data(), 8);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// A /proc/self/status field in kB, or 0. VmHWM counts the mapped tape pages
// too; RssAnon is the heap (books, pools, maps) alone.
static uint64_t status_kb(const std::string& field) {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind(field, 0) == 0) {
            return std::strtoull(line.c_str() + field.size(), nullptr, 10);
        }
    }
    return 0;
}

static bool parse_u64(const char* text, uint64_t& out) {
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    out = value;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <path-to-itch-file> [--memory-budget-mb N | --fixed-cap N] [--shards N]\n"
                  << "Default: size each locate's pool from pass 1 within 4096 MB. "
                     "--shards N (default 1) is one parser thread plus N book workers.\n";
        return 1;
    }
    const std::string path = argv[1];
    uint64_t budget_mb = 4096;
    uint64_t fixed_cap = 0;
    uint64_t shards = 1;
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << flag << "\n";
            return 1;
        }
        uint64_t* target = flag == "--memory-budget-mb" ? &budget_mb
                           : flag == "--fixed-cap"      ? &fixed_cap
                           : flag == "--shards"         ? &shards
                                                        : nullptr;
        if (!target || !parse_u64(argv[++i], *target) || (flag == "--shards" && *target == 0)) {
            std::cerr << "Bad argument: " << flag << " " << argv[i] << "\n";
            return 1;
        }
    }
    MmapReader reader(path);

    // --- Pass 1: stock_locate -> ticker map, and the per-locate upper bound
    //     on live orders (peak of adds minus deletes) for pool sizing. ---
    std::unordered_map<uint16_t, std::string> locate_to_ticker;
    LiveOrderPeak live_peak;
    ParserCallbacks directory_cb;
    directory_cb.on_stock_directory = [&](const StockDirectory& m) {
        locate_to_ticker[m.header.stock_locate] = trim(m.stock);
    };
    directory_cb.on_add_order = [&](const AddOrder& m) { live_peak.on_add(m.header.stock_locate); };
    directory_cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
        live_peak.on_add(m.header.stock_locate);
    };
    directory_cb.on_order_delete = [&](const OrderDelete& m) {
        live_peak.on_delete(m.header.stock_locate);
    };
    reader.process_all(directory_cb);
    std::cout << "Found " << locate_to_ticker.size() << " symbols.\n";

    // --- Pass 2: reconstruct every symbol's book, routed by stock_locate
    //     directly -- no string hashing in the hot path. See
    //     bench_lookup_cost.cpp for the measured ~2x improvement this
    //     gives over routing by ticker string (MultiSymbolBook). ---
    ShardedReplay market(static_cast<size_t>(shards), /*tick=*/100, /*band_pct=*/0.5,
                         /*max_orders_per_symbol=*/fixed_cap ? fixed_cap : 5000);
    CapacityPlan plan;
    if (fixed_cap == 0) {
        const uint64_t budget_slots = budget_mb * 1024 * 1024 / kBytesPerOrderEstimate;
        plan = plan_capacity(live_peak.peaks(), budget_slots);
        market.set_capacities(plan.capacity);
        std::cout << "Pool sizing: budget=" << budget_mb << " MB  slots=" << plan.total_slots
                  << "  estimate=" << plan.total_slots * kBytesPerOrderEstimate / (1024 * 1024)
                  << " MB  cut_locates=" << plan.cut_locates.size() << "\n";
    } else {
        std::cout << "Pool sizing: fixed " << fixed_cap << " orders per symbol\n";
    }
    std::cout << "Shards: " << market.shard_count()
              << " (parser thread + one worker per shard, locate % N)\n";

    ParserCallbacks cb;
    cb.on_add_order = [&](const AddOrder& m) {
        market.add_order(m.header.stock_locate, m.order_ref_number, m.buy_sell_indicator,
                         m.price_raw, m.shares);
    };
    cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
        market.add_order(m.header.stock_locate, m.order_ref_number, m.buy_sell_indicator,
                         m.price_raw, m.shares);
    };
    cb.on_order_cancel = [&](const OrderCancel& m) {
        market.cancel_shares(m.header.stock_locate, m.order_ref_number, m.cancelled_shares);
    };
    cb.on_order_delete = [&](const OrderDelete& m) {
        market.delete_order(m.header.stock_locate, m.order_ref_number);
    };
    cb.on_order_executed = [&](const OrderExecuted& m) {
        market.execute_shares(m.header.stock_locate, m.order_ref_number, m.executed_shares);
    };
    cb.on_order_executed_with_price = [&](const OrderExecutedWithPrice& m) {
        market.execute_shares(m.header.stock_locate, m.order_ref_number, m.executed_shares);
    };
    cb.on_order_replace = [&](const OrderReplace& m) {
        market.replace_order(m.header.stock_locate, m.original_order_ref_number,
                             m.new_order_ref_number, m.price_raw, m.shares);
    };

    market.start();
    auto t0 = std::chrono::steady_clock::now();
    size_t total = reader.process_all(cb);
    market.stop();
    auto t1 = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "\n=== Full market reconstruction (locate-indexed) ===\n";
    std::cout << "Total messages in file: " << total << "\n";
    std::cout << "Frames: short=" << reader.frame_stats().short_frames
              << " truncated_tail=" << reader.frame_stats().truncated_tail << "\n";
    const DropCounts drops = market.drop_counts();
    uint64_t applied = 0;
    for (const auto& [locate, ticker] : locate_to_ticker) {
        (void)ticker;
        applied += market.applied_events(locate);
    }
    std::cout << "Events applied across all symbols: " << applied << "\n";
    std::cout << "Drops: price_out_of_band=" << drops.price_out_of_band
              << " pool_exhausted=" << drops.pool_exhausted
              << " unknown_order=" << drops.unknown_order
              << " duplicate_order=" << drops.duplicate_order << "\n";
    std::cout << "Elapsed time: " << seconds << " s\n";
    std::cout << "Throughput: " << static_cast<double>(total) / seconds << " messages/sec\n";
    std::cout << "Parser ring spins (full): " << market.push_spins() << "\n\n";

    // Final resting size is a poor rank: the close leaves books empty.
    // Applied events are counted in the vector reserved above.
    std::vector<std::pair<std::string, uint16_t>> by_activity;
    std::vector<std::pair<std::string, uint16_t>> exhausted;
    for (const auto& [locate, ticker] : locate_to_ticker) {
        if (!market.has_locate(locate)) continue;
        by_activity.emplace_back(ticker, locate);
        if (market.locate_drops(locate).pool_exhausted > 0) {
            exhausted.emplace_back(ticker, locate);
        }
    }
    std::sort(by_activity.begin(), by_activity.end(), [&](const auto& a, const auto& b) {
        return market.applied_events(a.second) > market.applied_events(b.second);
    });
    std::sort(exhausted.begin(), exhausted.end(), [&](const auto& a, const auto& b) {
        return market.locate_drops(a.second).pool_exhausted >
               market.locate_drops(b.second).pool_exhausted;
    });

    std::cout << "Symbols with activity: " << by_activity.size() << "\n";
    std::cout << "Top 10 symbols by applied events:\n";
    std::cout << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < std::min<size_t>(10, by_activity.size()); ++i) {
        const auto& [ticker, locate] = by_activity[i];
        const DropCounts symbol_drops = market.locate_drops(locate);
        std::cout << "  " << ticker << ": applied=" << market.applied_events(locate)
                  << " pool_exhausted=" << symbol_drops.pool_exhausted
                  << " resting=" << market.resting_order_count(locate)
                  << " bid=" << market.best_bid(locate) << " ask=" << market.best_ask(locate)
                  << "\n";
    }
    std::cout << "Symbols that hit their pool cap: " << exhausted.size() << "\n";
    for (size_t i = 0; i < std::min<size_t>(10, exhausted.size()); ++i) {
        const auto& [ticker, locate] = exhausted[i];
        std::cout << "  " << ticker << ": pool_exhausted="
                  << market.locate_drops(locate).pool_exhausted
                  << " capacity=" << market.pool_capacity(locate)
                  << " pass1_peak=" << live_peak.peaks()[locate] << "\n";
    }
    if (!plan.cut_locates.empty()) {
        std::vector<uint16_t> cut = plan.cut_locates;
        std::sort(cut.begin(), cut.end(), [&](uint16_t a, uint16_t b) {
            return live_peak.peaks()[a] > live_peak.peaks()[b];
        });
        std::cout << "Locates cut by the memory budget (cap " << plan.cap << "): "
                  << cut.size() << "\n";
        for (size_t i = 0; i < std::min<size_t>(10, cut.size()); ++i) {
            const uint16_t locate = cut[i];
            auto it = locate_to_ticker.find(locate);
            std::cout << "  " << (it == locate_to_ticker.end() ? "?" : it->second)
                      << ": pass1_peak=" << live_peak.peaks()[locate] << "\n";
        }
    }
    std::cout << "Peak RSS (includes mapped tape): " << status_kb("VmHWM:") / 1024
              << " MB  heap RSS at end: " << status_kb("RssAnon:") / 1024 << " MB\n";

    return 0;
}
