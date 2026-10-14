#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#include "itch/mmap_reader.hpp"
#include "itch/parser.hpp"

using namespace itch;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file>\n";
        return 1;
    }
    const std::string path = argv[1];

    // Flat, O(1), cache-friendly histogram indexed directly by message type
    // byte -- replaces the std::map used in the first version, which cost
    // a red-black tree traversal per message just for bookkeeping.
    std::array<uint64_t, 256> type_counts{};
    uint64_t typed_messages = 0;

    ParserCallbacks cb;
    cb.on_any_message = [&](char type, uint16_t /*length*/) {
        ++type_counts[static_cast<unsigned char>(type)];
    };
    cb.on_add_order = [&](const AddOrder&) { ++typed_messages; };
    cb.on_add_order_mpid = [&](const AddOrderMPID&) { ++typed_messages; };
    cb.on_order_executed = [&](const OrderExecuted&) { ++typed_messages; };
    cb.on_order_executed_with_price = [&](const OrderExecutedWithPrice&) { ++typed_messages; };
    cb.on_order_cancel = [&](const OrderCancel&) { ++typed_messages; };
    cb.on_order_delete = [&](const OrderDelete&) { ++typed_messages; };
    cb.on_order_replace = [&](const OrderReplace&) { ++typed_messages; };
    cb.on_system_event = [&](const SystemEvent&) { ++typed_messages; };

    MmapReader reader(path);

    auto t0 = std::chrono::steady_clock::now();
    size_t count = reader.process_all(cb);
    auto t1 = std::chrono::steady_clock::now();

    double seconds = std::chrono::duration<double>(t1 - t0).count();
    double msgs_per_sec = static_cast<double>(count) / seconds;
    double mb_per_sec = (static_cast<double>(count) * 38.5 / (1024.0 * 1024.0)) / seconds;

    std::cout << "\n=== Message type histogram ===\n";
    for (int type = 0; type < 256; ++type) {
        if (type_counts[type] > 0) {
            std::cout << "  '" << static_cast<char>(type) << "' : "
                      << type_counts[type] << "\n";
        }
    }

    std::cout << "\n=== Summary ===\n";
    std::cout << "Total messages processed: " << count << "\n";
    std::cout << "Fully-typed (parsed into structs): " << typed_messages << "\n";
    std::cout << "Elapsed time: " << seconds << " s\n";
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "Throughput: " << msgs_per_sec << " messages/sec\n";
    std::cout << std::setprecision(1);
    std::cout << "Approx. throughput: " << mb_per_sec << " MB/s (avg frame size estimate)\n";

    return 0;
}
