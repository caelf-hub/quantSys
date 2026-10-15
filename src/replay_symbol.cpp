#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>

#include "book/order_book.hpp"
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
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file> [ticker]\n";
        std::cerr << "If no ticker is given, lists the first 20 symbols found.\n";
        return 1;
    }
    const std::string path = argv[1];
    const std::string requested_ticker = (argc >= 3) ? argv[2] : "";

    MmapReader reader(path);

    // --- Pass 1: build stock_locate -> ticker map from Stock Directory ---
    std::unordered_map<uint16_t, std::string> locate_to_ticker;
    ParserCallbacks directory_cb;
    directory_cb.on_stock_directory = [&](const StockDirectory& m) {
        locate_to_ticker[m.header.stock_locate] = trim(m.stock);
    };
    reader.process_all(directory_cb);

    std::cout << "Found " << locate_to_ticker.size() << " symbols in Stock Directory.\n";

    if (requested_ticker.empty()) {
        std::cout << "No ticker specified. First 20 symbols found:\n";
        int shown = 0;
        for (const auto& [locate, ticker] : locate_to_ticker) {
            std::cout << "  locate=" << locate << "  ticker=" << ticker << "\n";
            if (++shown >= 20) break;
        }
        std::cout << "\nRe-run with one of these tickers as the second argument.\n";
        return 0;
    }

    uint16_t target_locate = 0;
    bool found = false;
    for (const auto& [locate, ticker] : locate_to_ticker) {
        if (ticker == requested_ticker) {
            target_locate = locate;
            found = true;
            break;
        }
    }
    if (!found) {
        std::cerr << "Ticker '" << requested_ticker << "' not found in this file.\n";
        return 1;
    }
    std::cout << "Resolved '" << requested_ticker << "' to stock_locate=" << target_locate << "\n\n";

    // Wide default range to accommodate most equity prices; add_order()
    // safely rejects (returns false) anything outside this band rather
    // than corrupting state, so we just count rejects below.
    OrderBook ob(/*min=*/1 * 10000, /*max=*/2000 * 10000, /*tick=*/100, /*max_orders=*/2'000'000);

    uint64_t applied = 0;
    uint64_t rejected = 0;
    uint64_t snapshot_interval = 50'000;
    uint64_t events_seen = 0;

    auto maybe_snapshot = [&]() {
        if (++events_seen % snapshot_interval == 0) {
            std::cout << "[event " << events_seen << "] "
                      << "best_bid=" << std::fixed << std::setprecision(2) << ob.best_bid()
                      << "  best_ask=" << ob.best_ask()
                      << "  resting_orders=" << ob.resting_order_count() << "\n";
        }
    };

    ParserCallbacks cb;
    cb.on_add_order = [&](const AddOrder& m) {
        if (m.header.stock_locate != target_locate) return;
        bool ok = ob.add_order(m.order_ref_number, m.buy_sell_indicator, m.price_raw, m.shares);
        if (!ok) {
            std::cerr << "[REJECT A] price_raw=" << m.price_raw
                      << " shares=" << m.shares << " side=" << m.buy_sell_indicator
                      << " ref=" << m.order_ref_number << "\n";
        }
        ok ? ++applied : ++rejected;
        maybe_snapshot();
    };
    cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
        if (m.header.stock_locate != target_locate) return;
        bool ok = ob.add_order(m.order_ref_number, m.buy_sell_indicator, m.price_raw, m.shares);
        if (!ok) {
            std::cerr << "[REJECT F] price_raw=" << m.price_raw
                      << " shares=" << m.shares << " side=" << m.buy_sell_indicator
                      << " ref=" << m.order_ref_number << "\n";
        }
        ok ? ++applied : ++rejected;
        maybe_snapshot();
    };
    cb.on_order_cancel = [&](const OrderCancel& m) {
        if (m.header.stock_locate != target_locate) return;
        ob.cancel_shares(m.order_ref_number, m.cancelled_shares);
        ++applied;
        maybe_snapshot();
    };
    cb.on_order_delete = [&](const OrderDelete& m) {
        if (m.header.stock_locate != target_locate) return;
        ob.delete_order(m.order_ref_number);
        ++applied;
        maybe_snapshot();
    };
    cb.on_order_executed = [&](const OrderExecuted& m) {
        if (m.header.stock_locate != target_locate) return;
        ob.execute_shares(m.order_ref_number, m.executed_shares);
        ++applied;
        maybe_snapshot();
    };
    cb.on_order_executed_with_price = [&](const OrderExecutedWithPrice& m) {
        if (m.header.stock_locate != target_locate) return;
        ob.execute_shares(m.order_ref_number, m.executed_shares);
        ++applied;
        maybe_snapshot();
    };
    cb.on_order_replace = [&](const OrderReplace& m) {
        if (m.header.stock_locate != target_locate) return;
        ob.replace_order(m.original_order_ref_number, m.new_order_ref_number,
                          m.price_raw, m.shares);
        ++applied;
        maybe_snapshot();
    };

    size_t total = reader.process_all(cb);

    std::cout << "\n=== Final state for " << requested_ticker << " ===\n";
    std::cout << "Total messages in file: " << total << "\n";
    std::cout << "Frames: short=" << reader.frame_stats().short_frames
              << " truncated_tail=" << reader.frame_stats().truncated_tail << "\n";
    std::cout << "Events applied to this symbol: " << applied << "\n";
    std::cout << "Events rejected (price outside ladder range): " << rejected << "\n";
    std::cout << "Resting orders remaining: " << ob.resting_order_count() << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Final best bid: " << ob.best_bid() << "\n";
    std::cout << "Final best ask: " << ob.best_ask() << "\n";

    return 0;
}
