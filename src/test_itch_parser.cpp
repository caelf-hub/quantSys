#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "itch/mmap_reader.hpp"
#include "itch/parser.hpp"

using namespace itch;

static void write_be16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v));
}

static void write_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 24));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v));
}

static void write_be48(std::vector<uint8_t>& buf, uint64_t v) {
    for (int shift = 40; shift >= 0; shift -= 8)
        buf.push_back(static_cast<uint8_t>(v >> shift));
}

static void write_be64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
        buf.push_back(static_cast<uint8_t>(v >> shift));
}

static std::vector<uint8_t> build_add_order(uint16_t stock_locate, uint16_t tracking,
                                             uint64_t timestamp_ns, uint64_t order_ref,
                                             char side, uint32_t shares,
                                             const char* stock8, uint32_t price_raw) {
    std::vector<uint8_t> m;
    m.push_back('A');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    write_be64(m, order_ref);
    m.push_back(static_cast<uint8_t>(side));
    write_be32(m, shares);
    m.insert(m.end(), stock8, stock8 + 8);
    write_be32(m, price_raw);
    assert(m.size() == 36);
    return m;
}

static std::vector<uint8_t> build_add_order_mpid(uint16_t stock_locate, uint16_t tracking,
                                                  uint64_t timestamp_ns, uint64_t order_ref,
                                                  char side, uint32_t shares,
                                                  const char* stock8, uint32_t price_raw,
                                                  const char* attribution4) {
    std::vector<uint8_t> m;
    m.push_back('F');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    write_be64(m, order_ref);
    m.push_back(static_cast<uint8_t>(side));
    write_be32(m, shares);
    m.insert(m.end(), stock8, stock8 + 8);
    write_be32(m, price_raw);
    m.insert(m.end(), attribution4, attribution4 + 4);
    assert(m.size() == 40);
    return m;
}

static std::vector<uint8_t> build_order_executed(uint16_t stock_locate, uint16_t tracking,
                                                  uint64_t timestamp_ns, uint64_t order_ref,
                                                  uint32_t executed_shares, uint64_t match_number) {
    std::vector<uint8_t> m;
    m.push_back('E');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    write_be64(m, order_ref);
    write_be32(m, executed_shares);
    write_be64(m, match_number);
    assert(m.size() == 31);
    return m;
}

static std::vector<uint8_t> build_order_executed_with_price(
    uint16_t stock_locate, uint16_t tracking, uint64_t timestamp_ns, uint64_t order_ref,
    uint32_t executed_shares, uint64_t match_number, char printable, uint32_t exec_price_raw) {
    std::vector<uint8_t> m;
    m.push_back('C');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    write_be64(m, order_ref);
    write_be32(m, executed_shares);
    write_be64(m, match_number);
    m.push_back(static_cast<uint8_t>(printable));
    write_be32(m, exec_price_raw);
    assert(m.size() == 36);
    return m;
}

static std::vector<uint8_t> build_order_cancel(uint16_t stock_locate, uint16_t tracking,
                                                uint64_t timestamp_ns, uint64_t order_ref,
                                                uint32_t cancelled_shares) {
    std::vector<uint8_t> m;
    m.push_back('X');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    write_be64(m, order_ref);
    write_be32(m, cancelled_shares);
    assert(m.size() == 23);
    return m;
}

static std::vector<uint8_t> build_order_delete(uint16_t stock_locate, uint16_t tracking,
                                                uint64_t timestamp_ns, uint64_t order_ref) {
    std::vector<uint8_t> m;
    m.push_back('D');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    write_be64(m, order_ref);
    assert(m.size() == 19);
    return m;
}

static std::vector<uint8_t> build_order_replace(uint16_t stock_locate, uint16_t tracking,
                                                 uint64_t timestamp_ns, uint64_t orig_ref,
                                                 uint64_t new_ref, uint32_t shares,
                                                 uint32_t price_raw) {
    std::vector<uint8_t> m;
    m.push_back('U');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    write_be64(m, orig_ref);
    write_be64(m, new_ref);
    write_be32(m, shares);
    write_be32(m, price_raw);
    assert(m.size() == 35);
    return m;
}

static std::vector<uint8_t> build_system_event(uint16_t stock_locate, uint16_t tracking,
                                                uint64_t timestamp_ns, char event_code) {
    std::vector<uint8_t> m;
    m.push_back('S');
    write_be16(m, stock_locate);
    write_be16(m, tracking);
    write_be48(m, timestamp_ns);
    m.push_back(static_cast<uint8_t>(event_code));
    assert(m.size() == 12);
    return m;
}

static void write_framed(std::ofstream& out, const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> len_prefix;
    write_be16(len_prefix, static_cast<uint16_t>(msg.size()));
    out.write(reinterpret_cast<const char*>(len_prefix.data()), len_prefix.size());
    out.write(reinterpret_cast<const char*>(msg.data()), msg.size());
}

int main() {
    const std::string path = "test_itch_sample.bin";

    auto add_msg = build_add_order(42, 1, 100000000000ULL, 1001, 'B', 500, "AAPL    ", 1500000);
    auto add_mpid_msg = build_add_order_mpid(42, 2, 100000000100ULL, 1002, 'S', 300,
                                              "MSFT    ", 3250000, "NSDQ");
    auto executed_msg = build_order_executed(42, 3, 100000000200ULL, 1001, 200, 55555);
    auto executed_price_msg = build_order_executed_with_price(42, 4, 100000000300ULL, 1002,
                                                                100, 55556, 'Y', 3251000);
    auto cancel_msg = build_order_cancel(42, 5, 100000000400ULL, 1001, 100);
    auto delete_msg = build_order_delete(42, 6, 100000000500ULL, 1001);
    auto replace_msg = build_order_replace(42, 7, 100000000600ULL, 1002, 2002, 400, 3300000);
    auto system_msg = build_system_event(0, 8, 100000000700ULL, 'Q');

    {
        std::ofstream out(path, std::ios::binary);
        write_framed(out, add_msg);
        write_framed(out, add_mpid_msg);
        write_framed(out, executed_msg);
        write_framed(out, executed_price_msg);
        write_framed(out, cancel_msg);
        write_framed(out, delete_msg);
        write_framed(out, replace_msg);
        write_framed(out, system_msg);
    }

    int seen = 0;
    ParserCallbacks cb;

    cb.on_add_order = [&](const AddOrder& m) {
        assert(m.header.stock_locate == 42 && m.header.tracking_number == 1);
        assert(m.header.timestamp_ns == 100000000000ULL);
        assert(m.order_ref_number == 1001 && m.buy_sell_indicator == 'B');
        assert(m.shares == 500);
        assert(std::memcmp(m.stock.data(), "AAPL    ", 8) == 0);
        assert(m.price_raw == 1500000 && price_to_double(m.price_raw) == 150.0);
        std::cout << "[PASS] AddOrder\n";
        ++seen;
    };

    cb.on_add_order_mpid = [&](const AddOrderMPID& m) {
        assert(m.header.stock_locate == 42 && m.header.tracking_number == 2);
        assert(m.order_ref_number == 1002 && m.buy_sell_indicator == 'S');
        assert(m.shares == 300);
        assert(std::memcmp(m.stock.data(), "MSFT    ", 8) == 0);
        assert(m.price_raw == 3250000 && price_to_double(m.price_raw) == 325.0);
        assert(std::memcmp(m.attribution.data(), "NSDQ", 4) == 0);
        std::cout << "[PASS] AddOrderMPID\n";
        ++seen;
    };

    cb.on_order_executed = [&](const OrderExecuted& m) {
        assert(m.header.tracking_number == 3);
        assert(m.order_ref_number == 1001);
        assert(m.executed_shares == 200);
        assert(m.match_number == 55555);
        std::cout << "[PASS] OrderExecuted\n";
        ++seen;
    };

    cb.on_order_executed_with_price = [&](const OrderExecutedWithPrice& m) {
        assert(m.header.tracking_number == 4);
        assert(m.order_ref_number == 1002);
        assert(m.executed_shares == 100);
        assert(m.match_number == 55556);
        assert(m.printable == 'Y');
        assert(m.execution_price_raw == 3251000 && price_to_double(m.execution_price_raw) == 325.1);
        std::cout << "[PASS] OrderExecutedWithPrice\n";
        ++seen;
    };

    cb.on_order_cancel = [&](const OrderCancel& m) {
        assert(m.header.tracking_number == 5);
        assert(m.order_ref_number == 1001);
        assert(m.cancelled_shares == 100);
        std::cout << "[PASS] OrderCancel\n";
        ++seen;
    };

    cb.on_order_delete = [&](const OrderDelete& m) {
        assert(m.header.tracking_number == 6);
        assert(m.order_ref_number == 1001);
        std::cout << "[PASS] OrderDelete\n";
        ++seen;
    };

    cb.on_order_replace = [&](const OrderReplace& m) {
        assert(m.header.tracking_number == 7);
        assert(m.original_order_ref_number == 1002);
        assert(m.new_order_ref_number == 2002);
        assert(m.shares == 400);
        assert(m.price_raw == 3300000 && price_to_double(m.price_raw) == 330.0);
        std::cout << "[PASS] OrderReplace\n";
        ++seen;
    };

    cb.on_system_event = [&](const SystemEvent& m) {
        assert(m.header.stock_locate == 0 && m.header.tracking_number == 8);
        assert(m.event_code == 'Q');
        std::cout << "[PASS] SystemEvent\n";
        ++seen;
    };

    MmapReader reader(path);
    size_t count = reader.process_all(cb);

    assert(count == 8);
    assert(seen == 8);

    std::cout << "All " << count << " ITCH message types parsed and verified correctly.\n";
    return 0;
}
