#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>

#include "book/multi_symbol_book.hpp"
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

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file>\n";
        return 1;
    }
    const std::string path = argv[1];
    MmapReader reader(path);

    std::unordered_map<uint16_t, std::string> locate_to_ticker;
    ParserCallbacks directory_cb;
    directory_cb.on_stock_directory = [&](const StockDirectory& m) {
        locate_to_ticker[m.header.stock_locate] = trim(m.stock);
    };
    reader.process_all(directory_cb);
    std::cout << "Symbols: " << locate_to_ticker.size() << "\n\n";

    // --- Stage 1: baseline, no lookups at all (matches replay_itch) ---
    {
        volatile uint64_t sink = 0;
        ParserCallbacks cb;
        cb.on_add_order = [&](const AddOrder& m) { sink = sink + m.shares; };
        cb.on_add_order_mpid = [&](const AddOrderMPID& m) { sink = sink + m.shares; };

        double t0 = now_seconds();
        size_t total = reader.process_all(cb);
        double elapsed = now_seconds() - t0;
        std::cout << "Stage 1 (baseline, no lookups):          "
                  << std::fixed << std::setprecision(2)
                  << (total / elapsed / 1e6) << " M msgs/sec  ("
                  << elapsed << " s)\n";
    }

    // --- Stage 2: + locate -> ticker string lookup (unordered_map<uint16_t,string>) ---
    {
        volatile size_t sink = 0;
        ParserCallbacks cb;
        cb.on_add_order = [&](const AddOrder& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            sink = sink + sym.size();
        };
        cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            sink = sink + sym.size();
        };

        double t0 = now_seconds();
        size_t total = reader.process_all(cb);
        double elapsed = now_seconds() - t0;
        std::cout << "Stage 2 (+ locate->ticker lookup):        "
                  << (total / elapsed / 1e6) << " M msgs/sec  ("
                  << elapsed << " s)\n";
    }

    // --- Stage 3: + second lookup, string-keyed (unordered_map<string,OrderBook>::find) ---
    {
        MultiSymbolBook market(100, 0.5, 5000);
        // Pre-touch every symbol once so .find() below measures lookup
        // cost on an already-populated map, isolating lookup from the
        // one-time book-creation cost (which Stage 4 measures separately).
        for (const auto& [locate, ticker] : locate_to_ticker) {
            market.add_order(ticker, 0, 'B', 1 * 10000, 1);
        }

        volatile size_t sink = 0;
        ParserCallbacks cb;
        cb.on_add_order = [&](const AddOrder& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            sink = sink + market.resting_order_count(sym); // triggers find(), discards result
        };
        cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            sink = sink + market.resting_order_count(sym);
        };

        double t0 = now_seconds();
        size_t total = reader.process_all(cb);
        double elapsed = now_seconds() - t0;
        std::cout << "Stage 3 (+ string-keyed book lookup):     "
                  << (total / elapsed / 1e6) << " M msgs/sec  ("
                  << elapsed << " s)\n";
    }

    // --- Stage 4: full real work, same as replay_full_market ---
    {
        MultiSymbolBook market(100, 0.5, 5000);
        ParserCallbacks cb;
        cb.on_add_order = [&](const AddOrder& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.add_order(sym, m.order_ref_number, m.buy_sell_indicator,
                              m.price_raw, m.shares);
        };
        cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.add_order(sym, m.order_ref_number, m.buy_sell_indicator,
                              m.price_raw, m.shares);
        };

        double t0 = now_seconds();
        size_t total = reader.process_all(cb);
        double elapsed = now_seconds() - t0;
        std::cout << "Stage 4 (+ full book mutation):           "
                  << (total / elapsed / 1e6) << " M msgs/sec  ("
                  << elapsed << " s)\n";
    }


    // --- Stage 5: locate-indexed book (flat array, no string hashing) ---
    {
        LocateIndexedBook market(100, 0.5, 5000);
        ParserCallbacks cb;
        cb.on_add_order = [&](const AddOrder& m) {
            market.add_order(m.header.stock_locate, m.order_ref_number,
                              m.buy_sell_indicator, m.price_raw, m.shares);
        };
        cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
            market.add_order(m.header.stock_locate, m.order_ref_number,
                              m.buy_sell_indicator, m.price_raw, m.shares);
        };

        double t0 = now_seconds();
        size_t total = reader.process_all(cb);
        double elapsed = now_seconds() - t0;
        std::cout << "Stage 5 (locate-indexed, no string hashing): "
                  << (total / elapsed / 1e6) << " M msgs/sec  ("
                  << elapsed << " s)\n";
    }

    // --- Stage 6: full real message mix, string-keyed (MultiSymbolBook) ---
    {
        MultiSymbolBook market(100, 0.5, 5000);
        ParserCallbacks cb;
        cb.on_add_order = [&](const AddOrder& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.add_order(sym, m.order_ref_number, m.buy_sell_indicator, m.price_raw, m.shares);
        };
        cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.add_order(sym, m.order_ref_number, m.buy_sell_indicator, m.price_raw, m.shares);
        };
        cb.on_order_cancel = [&](const OrderCancel& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.cancel_shares(sym, m.order_ref_number, m.cancelled_shares);
        };
        cb.on_order_delete = [&](const OrderDelete& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.delete_order(sym, m.order_ref_number);
        };
        cb.on_order_executed = [&](const OrderExecuted& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.execute_shares(sym, m.order_ref_number, m.executed_shares);
        };
        cb.on_order_executed_with_price = [&](const OrderExecutedWithPrice& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.execute_shares(sym, m.order_ref_number, m.executed_shares);
        };
        cb.on_order_replace = [&](const OrderReplace& m) {
            const std::string& sym = locate_to_ticker[m.header.stock_locate];
            market.replace_order(sym, m.original_order_ref_number, m.new_order_ref_number,
                                   m.price_raw, m.shares);
        };

        double t0 = now_seconds();
        size_t total = reader.process_all(cb);
        double elapsed = now_seconds() - t0;
        std::cout << "Stage 6 (full mix, string-keyed):            "
                  << (total / elapsed / 1e6) << " M msgs/sec  ("
                  << elapsed << " s)\n";
    }

    // --- Stage 7: full real message mix, locate-indexed ---
    {
        LocateIndexedBook market(100, 0.5, 5000);
        ParserCallbacks cb;
        cb.on_add_order = [&](const AddOrder& m) {
            market.add_order(m.header.stock_locate, m.order_ref_number,
                              m.buy_sell_indicator, m.price_raw, m.shares);
        };
        cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
            market.add_order(m.header.stock_locate, m.order_ref_number,
                              m.buy_sell_indicator, m.price_raw, m.shares);
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

        double t0 = now_seconds();
        size_t total = reader.process_all(cb);
        double elapsed = now_seconds() - t0;
        std::cout << "Stage 7 (full mix, locate-indexed):          "
                  << (total / elapsed / 1e6) << " M msgs/sec  ("
                  << elapsed << " s)\n";
    }
    return 0;
}
