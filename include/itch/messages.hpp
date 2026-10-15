#pragma once

#include <array>
#include <cstring>
#include <cstdint>
#include <string_view>

#include "byteswap.hpp"

namespace itch {

// Common header fields present at the start of every message, per spec.
struct Header {
    char message_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns; // nanoseconds since midnight
};

inline Header parse_header(const uint8_t* buf) {
    Header h;
    h.message_type = static_cast<char>(buf[0]);
    h.stock_locate = read_be16(buf + 1);
    h.tracking_number = read_be16(buf + 3);
    h.timestamp_ns = read_be48(buf + 5);
    return h;
}

struct SystemEvent {
    Header header;
    char event_code;
};

inline SystemEvent parse_system_event(const uint8_t* buf) {
    SystemEvent m;
    m.header = parse_header(buf);
    m.event_code = static_cast<char>(buf[11]);
    return m;
}

// Message type 'A' — Add Order, No MPID Attribution.
struct AddOrder {
    Header header;
    uint64_t order_ref_number;
    char buy_sell_indicator; // 'B' or 'S'
    uint32_t shares;
    std::array<char, 8> stock;
    uint32_t price_raw; // fixed point, 4 implied decimal places
};

inline AddOrder parse_add_order(const uint8_t* buf) {
    AddOrder m;
    m.header = parse_header(buf);
    m.order_ref_number = read_be64(buf + 11);
    m.buy_sell_indicator = static_cast<char>(buf[19]);
    m.shares = read_be32(buf + 20);
    std::memcpy(m.stock.data(), buf + 24, 8);
    m.price_raw = read_be32(buf + 32);
    return m;
}

// Message type 'F' — Add Order with MPID Attribution.
struct AddOrderMPID {
    Header header;
    uint64_t order_ref_number;
    char buy_sell_indicator;
    uint32_t shares;
    std::array<char, 8> stock;
    uint32_t price_raw;
    std::array<char, 4> attribution;
};

inline AddOrderMPID parse_add_order_mpid(const uint8_t* buf) {
    AddOrderMPID m;
    m.header = parse_header(buf);
    m.order_ref_number = read_be64(buf + 11);
    m.buy_sell_indicator = static_cast<char>(buf[19]);
    m.shares = read_be32(buf + 20);
    std::memcpy(m.stock.data(), buf + 24, 8);
    m.price_raw = read_be32(buf + 32);
    std::memcpy(m.attribution.data(), buf + 36, 4);
    return m;
}

// Message type 'E' — Order Executed.
struct OrderExecuted {
    Header header;
    uint64_t order_ref_number;
    uint32_t executed_shares;
    uint64_t match_number;
};

inline OrderExecuted parse_order_executed(const uint8_t* buf) {
    OrderExecuted m;
    m.header = parse_header(buf);
    m.order_ref_number = read_be64(buf + 11);
    m.executed_shares = read_be32(buf + 19);
    m.match_number = read_be64(buf + 23);
    return m;
}

// Message type 'C' — Order Executed With Price.
struct OrderExecutedWithPrice {
    Header header;
    uint64_t order_ref_number;
    uint32_t executed_shares;
    uint64_t match_number;
    char printable; // 'Y' or 'N'
    uint32_t execution_price_raw;
};

inline OrderExecutedWithPrice parse_order_executed_with_price(const uint8_t* buf) {
    OrderExecutedWithPrice m;
    m.header = parse_header(buf);
    m.order_ref_number = read_be64(buf + 11);
    m.executed_shares = read_be32(buf + 19);
    m.match_number = read_be64(buf + 23);
    m.printable = static_cast<char>(buf[31]);
    m.execution_price_raw = read_be32(buf + 32);
    return m;
}

// Message type 'X' — Order Cancel (partial).
struct OrderCancel {
    Header header;
    uint64_t order_ref_number;
    uint32_t cancelled_shares;
};

inline OrderCancel parse_order_cancel(const uint8_t* buf) {
    OrderCancel m;
    m.header = parse_header(buf);
    m.order_ref_number = read_be64(buf + 11);
    m.cancelled_shares = read_be32(buf + 19);
    return m;
}

// Message type 'D' — Order Delete (full cancel).
struct OrderDelete {
    Header header;
    uint64_t order_ref_number;
};

inline OrderDelete parse_order_delete(const uint8_t* buf) {
    OrderDelete m;
    m.header = parse_header(buf);
    m.order_ref_number = read_be64(buf + 11);
    return m;
}

// Message type 'U' — Order Replace (cancel-replace).
struct OrderReplace {
    Header header;
    uint64_t original_order_ref_number;
    uint64_t new_order_ref_number;
    uint32_t shares;
    uint32_t price_raw;
};

inline OrderReplace parse_order_replace(const uint8_t* buf) {
    OrderReplace m;
    m.header = parse_header(buf);
    m.original_order_ref_number = read_be64(buf + 11);
    m.new_order_ref_number = read_be64(buf + 19);
    m.shares = read_be32(buf + 27);
    m.price_raw = read_be32(buf + 31);
    return m;
}

// Message type 'P' -- Trade (non-cross). An execution against a
// non-displayed order. It never touches the displayed book. The order
// reference is zeroed in the public feed.
struct Trade {
    Header header;
    uint64_t order_ref_number;
    char buy_sell_indicator; // side of the resting non-displayed order
    uint32_t shares;
    std::array<char, 8> stock;
    uint32_t price_raw;
    uint64_t match_number;
};

inline Trade parse_trade(const uint8_t* buf) {
    Trade m;
    m.header = parse_header(buf);
    m.order_ref_number = read_be64(buf + 11);
    m.buy_sell_indicator = static_cast<char>(buf[19]);
    m.shares = read_be32(buf + 20);
    std::memcpy(m.stock.data(), buf + 24, 8);
    m.price_raw = read_be32(buf + 32);
    m.match_number = read_be64(buf + 36);
    return m;
}

// Price fields have an implied 4 decimal places (per spec Data Types section).
inline double price_to_double(uint32_t price_raw) {
    return static_cast<double>(price_raw) / 10000.0;
}

// Message type 'R' -- Stock Directory. Sent once per active symbol at
// the start of the trading day; gives the stock_locate -> ticker mapping
// that every other message type relies on (they only carry stock_locate,
// not the ticker string itself).
struct StockDirectory {
    Header header;
    std::array<char, 8> stock;
    char market_category;
    char financial_status_indicator;
    uint32_t round_lot_size;
    char round_lots_only;
};

inline StockDirectory parse_stock_directory(const uint8_t* buf) {
    StockDirectory m;
    m.header = parse_header(buf);
    std::memcpy(m.stock.data(), buf + 11, 8);
    m.market_category = static_cast<char>(buf[19]);
    m.financial_status_indicator = static_cast<char>(buf[20]);
    m.round_lot_size = read_be32(buf + 21);
    m.round_lots_only = static_cast<char>(buf[25]);
    return m;
}

} // namespace itch
