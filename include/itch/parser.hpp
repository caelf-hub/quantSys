#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "byteswap.hpp"
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
    std::function<void(const Trade&)> on_trade;

    // Fires for every message, regardless of type, including ones we have
    // no typed struct for yet (e.g. NOII 'I', Cross Trade 'Q').
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
        case 'P': return 44;
        default: return -1; // unknown / not yet modeled
    }
}

enum class DispatchResult { Handled, Unknown, ShortFrame };

// Dispatches a single message buffer (starting at the Message Type byte)
// to the appropriate callback. `declared_len` is the length from the
// 2-byte frame prefix and must be at least 1. A frame shorter than its
// type's fixed length is not parsed: the typed parsers read fixed offsets
// and would run past the frame. A longer frame parses its known prefix.
inline DispatchResult dispatch_message(const uint8_t* buf, uint16_t declared_len,
                                        const ParserCallbacks& cb) {
    char type = static_cast<char>(buf[0]);

    if (cb.on_any_message) cb.on_any_message(type, declared_len);

    const int needed = message_length(type);
    if (needed < 0) return DispatchResult::Unknown;
    if (declared_len < needed) return DispatchResult::ShortFrame;

    switch (type) {
        case 'A':
            if (cb.on_add_order) cb.on_add_order(parse_add_order(buf));
            break;
        case 'F':
            if (cb.on_add_order_mpid) cb.on_add_order_mpid(parse_add_order_mpid(buf));
            break;
        case 'E':
            if (cb.on_order_executed) cb.on_order_executed(parse_order_executed(buf));
            break;
        case 'C':
            if (cb.on_order_executed_with_price)
                cb.on_order_executed_with_price(parse_order_executed_with_price(buf));
            break;
        case 'X':
            if (cb.on_order_cancel) cb.on_order_cancel(parse_order_cancel(buf));
            break;
        case 'D':
            if (cb.on_order_delete) cb.on_order_delete(parse_order_delete(buf));
            break;
        case 'U':
            if (cb.on_order_replace) cb.on_order_replace(parse_order_replace(buf));
            break;
        case 'S':
            if (cb.on_system_event) cb.on_system_event(parse_system_event(buf));
            break;
        case 'R':
            if (cb.on_stock_directory) cb.on_stock_directory(parse_stock_directory(buf));
            break;
        case 'P':
            if (cb.on_trade) cb.on_trade(parse_trade(buf));
            break;
        default:
            return DispatchResult::Unknown;
    }
    return DispatchResult::Handled;
}

// Frames the walk could not parse. A short frame is skipped by its
// declared length. A truncated tail (length past the end of the buffer,
// or a zero length) stops the walk.
struct FrameStats {
    uint64_t short_frames = 0;
    uint64_t truncated_tail = 0;
};

// Walks 2-byte big-endian length-prefixed frames. Returns the number of
// complete frames dispatched, including short and unknown ones.
inline size_t process_buffer(const uint8_t* data, size_t size, const ParserCallbacks& cb,
                             FrameStats* stats = nullptr) {
    size_t offset = 0;
    size_t count = 0;
    while (offset + 2 <= size) {
        const uint16_t msg_len = read_be16(data + offset);
        if (msg_len == 0 || offset + 2 + msg_len > size) {
            if (stats) ++stats->truncated_tail;
            break;
        }
        offset += 2;
        if (dispatch_message(data + offset, msg_len, cb) == DispatchResult::ShortFrame) {
            if (stats) ++stats->short_frames;
        }
        offset += msg_len;
        ++count;
    }
    if (offset < size && offset + 2 > size && stats) ++stats->truncated_tail;
    return count;
}

} // namespace itch
