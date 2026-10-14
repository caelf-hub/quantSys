#pragma once

#include <cstdint>
#include <functional>

#include "messages.hpp"

namespace itch {

// Callbacks the caller supplies; only the ones relevant get invoked.
struct ParserCallbacks {
    std::function<void(const AddOrder&)> on_add_order;
    std::function<void(const AddOrderMPID&)> on_add_order_mpid;
    std::function<void(const OrderExecuted&)> on_order_executed;
    std::function<void(const OrderExecutedWithPrice&)> on_order_executed_with_price;
    std::function<void(const OrderCancel&)> on_order_cancel;
    std::function<void(const OrderDelete&)> on_order_delete;
    std::function<void(const OrderReplace&)> on_order_replace;
    std::function<void(const SystemEvent&)> on_system_event;
    std::function<void(const StockDirectory&)> on_stock_directory;

    // Fires for every message, regardless of type, including ones we have
    // no typed struct for yet (e.g. Stock Directory 'R', Trade 'P', NOII 'I').
    // Useful for building a message-type histogram over real market data.
    std::function<void(char type, uint16_t length)> on_any_message;
};

// Returns the byte length of a message given its type, per spec.
// Needed because messages are variable length and we must know how far
// to advance when NOT using the 2-byte SoupBinTCP length prefix framing.
inline int message_length(char type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        default: return -1; // unknown / not yet modeled
    }
}

// Dispatches a single message buffer (starting at the Message Type byte)
// to the appropriate callback. `declared_len` is the length from the
// 2-byte frame prefix, used for on_any_message bookkeeping even for
// message types we don't have a parser for. Returns false if the type
// is unrecognized by our typed parsers (on_any_message still fires).
inline bool dispatch_message(const uint8_t* buf, uint16_t declared_len,
                              const ParserCallbacks& cb) {
    char type = static_cast<char>(buf[0]);

    if (cb.on_any_message) cb.on_any_message(type, declared_len);

    switch (type) {
        case 'A':
            if (cb.on_add_order) cb.on_add_order(parse_add_order(buf));
            return true;
        case 'F':
            if (cb.on_add_order_mpid) cb.on_add_order_mpid(parse_add_order_mpid(buf));
            return true;
        case 'E':
            if (cb.on_order_executed) cb.on_order_executed(parse_order_executed(buf));
            return true;
        case 'C':
            if (cb.on_order_executed_with_price)
                cb.on_order_executed_with_price(parse_order_executed_with_price(buf));
            return true;
        case 'X':
            if (cb.on_order_cancel) cb.on_order_cancel(parse_order_cancel(buf));
            return true;
        case 'D':
            if (cb.on_order_delete) cb.on_order_delete(parse_order_delete(buf));
            return true;
        case 'U':
            if (cb.on_order_replace) cb.on_order_replace(parse_order_replace(buf));
            return true;
        case 'S':
            if (cb.on_system_event) cb.on_system_event(parse_system_event(buf));
            return true;
        case 'R':
            if (cb.on_stock_directory) cb.on_stock_directory(parse_stock_directory(buf));
            return true;
        default:
            return false; // unhandled message type, caller may choose to skip
    }
}

} // namespace itch
