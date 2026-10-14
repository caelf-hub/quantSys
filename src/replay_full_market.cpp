#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/locate_indexed_book.hpp"
#include "itch/mmap_reader.hpp"
#include "itch/parser.hpp"

using namespace itch;
using namespace book;

static std::string trim(const std::array<char, 8>& raw) {
    std::string s(raw.data(), 8);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file>\n";
        return 1;
    }
    const std::string path = argv[1];
    MmapReader reader(path);

    // --- Pass 1: build stock_locate -> ticker map (for the report only) ---
    std::unordered_map<uint16_t, std::string> locate_to_ticker;
    ParserCallbacks directory_cb;
    directory_cb.on_stock_directory = [&](const StockDirectory& m) {
        locate_to_ticker[m.header.stock_locate] = trim(m.stock);
    };
    reader.process_all(directory_cb);
    std::cout << "Found " << locate_to_ticker.size() << " symbols.\n";

    // --- Pass 2: reconstruct every symbol's book, routed by stock_locate
    //     directly -- no string hashing in the hot path. See
    //     bench_lookup_cost.cpp for the measured ~2x improvement this
    //     gives over routing by ticker string (MultiSymbolBook). ---
    LocateIndexedBook market(/*tick=*/100, /*band_pct=*/0.5, /*max_orders_per_symbol=*/5000);

    uint64_t applied = 0;
    ParserCallbacks cb;

    cb.on_add_order = [&](const AddOrder& m) {
        if (market.add_order(m.header.stock_locate, m.order_ref_number,
                               m.buy_sell_indicator, m.price_raw, m.shares)) {
            ++applied;
        }
    };
    cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
        if (market.add_order(m.header.stock_locate, m.order_ref_number,
                               m.buy_sell_indicator, m.price_raw, m.shares)) {
            ++applied;
        }
    };
    cb.on_order_cancel = [&](const OrderCancel& m) {
        if (market.cancel_shares(m.header.stock_locate, m.order_ref_number, m.cancelled_shares))
            ++applied;
    };
    cb.on_order_delete = [&](const OrderDelete& m) {
        if (market.delete_order(m.header.stock_locate, m.order_ref_number)) ++applied;
    };
    cb.on_order_executed = [&](const OrderExecuted& m) {
        if (market.execute_shares(m.header.stock_locate, m.order_ref_number, m.executed_shares))
            ++applied;
    };
    cb.on_order_executed_with_price = [&](const OrderExecutedWithPrice& m) {
        if (market.execute_shares(m.header.stock_locate, m.order_ref_number, m.executed_shares))
            ++applied;
    };
    cb.on_order_replace = [&](const OrderReplace& m) {
        if (market.replace_order(m.header.stock_locate, m.original_order_ref_number,
                                   m.new_order_ref_number, m.price_raw, m.shares)) {
            ++applied;
        }
    };

    auto t0 = std::chrono::steady_clock::now();
    size_t total = reader.process_all(cb);
    auto t1 = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "\n=== Full market reconstruction (locate-indexed) ===\n";
    std::cout << "Total messages in file: " << total << "\n";
    std::cout << "Events applied across all symbols: " << applied << "\n";
    std::cout << "Elapsed time: " << seconds << " s\n";
    std::cout << "Throughput: " << static_cast<double>(total) / seconds << " messages/sec\n\n";

    // --- Report: resolve ticker names only here, outside the hot path ---
    std::vector<std::pair<std::string, uint16_t>> by_activity;
    for (const auto& [locate, ticker] : locate_to_ticker) {
        if (market.has_locate(locate)) {
            by_activity.emplace_back(ticker, locate);
        }
    }
    std::sort(by_activity.begin(), by_activity.end(), [&](const auto& a, const auto& b) {
        return market.resting_order_count(a.second) > market.resting_order_count(b.second);
    });

    std::cout << "Symbols with activity: " << by_activity.size() << "\n";
    std::cout << "Top 10 symbols by final resting order count:\n";
    std::cout << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < std::min<size_t>(10, by_activity.size()); ++i) {
        const auto& [ticker, locate] = by_activity[i];
        std::cout << "  " << ticker << ": " << market.resting_order_count(locate) << " resting, "
                  << "bid=" << market.best_bid(locate) << " ask=" << market.best_ask(locate) << "\n";
    }

    return 0;
}
