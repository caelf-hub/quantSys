// Deterministic mutation fuzz for the ITCH frame walk and the book behind it.
// Meant to run under -DQUANT_SANITIZE=ON: the assertion is "no ASan/UBSan
// report", plus a few invariants that must hold for any input.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "book/locate_indexed_book.hpp"
#include "itch/parser.hpp"

using namespace itch;
using namespace book;

namespace {

constexpr size_t kMaxLocate = 16;
constexpr size_t kOrdersPerSymbol = 64;
constexpr int kIterations = 50000;
constexpr char kTypes[] = {'S', 'R', 'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P', 'Q', 'Z'};

struct Rng {
    std::mt19937_64 engine{0x5eed1234ULL};
    uint64_t next() { return engine(); }
    uint64_t below(uint64_t n) { return n == 0 ? 0 : next() % n; }
};

// Mostly a 20-tick window around $100, with zero, near-uint32-max, and
// arbitrary prices mixed in to exercise the band edges.
uint64_t random_price(Rng& rng) {
    switch (rng.below(16)) {
        case 0: return 0;
        case 1: return UINT32_MAX - rng.below(1000);
        case 2:
        case 3: return rng.next();
        default: return 1000000 + rng.below(20) * 100;
    }
}

void put_be(std::vector<uint8_t>& frame, size_t offset, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        if (offset + static_cast<size_t>(i) < frame.size()) {
            frame[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(value & 0xFF);
        }
        value >>= 8;
    }
}

// A frame of a random type whose fields mostly land in a small, plausible
// space so adds, cancels, and replaces actually hit live orders.
std::vector<uint8_t> random_frame(Rng& rng) {
    const char type = kTypes[rng.below(sizeof(kTypes))];
    const int known = message_length(type);
    size_t len = known > 0 ? static_cast<size_t>(known) : 1 + rng.below(48);
    if (rng.below(8) == 0) len = 1 + rng.below(len + 8);

    std::vector<uint8_t> frame(len);
    for (auto& b : frame) b = static_cast<uint8_t>(rng.next());
    frame[0] = static_cast<uint8_t>(type);
    put_be(frame, 1, rng.below(kMaxLocate + 2), 2);
    const uint64_t ref = 1 + rng.below(200);
    put_be(frame, 11, ref, 8);
    switch (type) {
        case 'A':
        case 'F':
        case 'P':
            put_be(frame, 19, rng.below(2) ? 'B' : 'S', 1);
            put_be(frame, 20, rng.below(5000), 4);
            put_be(frame, 32, random_price(rng), 4);
            break;
        case 'E':
        case 'C':
        case 'X':
            put_be(frame, 19, rng.below(6000), 4);
            break;
        case 'U':
            put_be(frame, 19, 1 + rng.below(200), 8);
            put_be(frame, 27, rng.below(5000), 4);
            put_be(frame, 31, random_price(rng), 4);
            break;
        default:
            break;
    }
    return frame;
}

std::vector<uint8_t> random_tape(Rng& rng) {
    std::vector<uint8_t> tape;
    const size_t frames = 1 + rng.below(24);
    for (size_t i = 0; i < frames; ++i) {
        const auto frame = random_frame(rng);
        tape.push_back(static_cast<uint8_t>(frame.size() >> 8));
        tape.push_back(static_cast<uint8_t>(frame.size() & 0xFF));
        tape.insert(tape.end(), frame.begin(), frame.end());
    }
    const size_t flips = rng.below(4);
    for (size_t i = 0; i < flips && !tape.empty(); ++i) {
        tape[rng.below(tape.size())] = static_cast<uint8_t>(rng.next());
    }
    if (rng.below(6) == 0 && !tape.empty()) tape.resize(rng.below(tape.size()));
    return tape;
}

} // namespace

int main() {
    Rng rng;
    LocateIndexedBook market(/*tick=*/100, /*band_pct=*/0.5, kOrdersPerSymbol, kMaxLocate);

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

    uint64_t trades = 0;
    uint64_t trade_shares = 0;
    cb.on_trade = [&](const Trade& m) {
        ++trades;
        trade_shares += m.shares;
    };

    uint64_t frames = 0;
    FrameStats stats;
    for (int i = 0; i < kIterations; ++i) {
        // Exact-size heap copy so any read past the tape is an ASan report.
        const auto tape = random_tape(rng);
        std::unique_ptr<uint8_t[]> exact(new uint8_t[tape.empty() ? 1 : tape.size()]);
        if (!tape.empty()) std::memcpy(exact.get(), tape.data(), tape.size());
        frames += process_buffer(exact.get(), tape.size(), cb, &stats);

        for (uint16_t locate = 0; locate < kMaxLocate; ++locate) {
            assert(market.resting_order_count(locate) <= kOrdersPerSymbol);
            const double bid = market.best_bid(locate);
            const double ask = market.best_ask(locate);
            assert(bid == -1.0 || bid > 0.0);
            assert(ask == -1.0 || ask > 0.0);
        }
    }

    const DropCounts drops = market.drop_counts();
    std::cout << "iterations=" << kIterations << " frames=" << frames
              << " short=" << stats.short_frames << " truncated_tail=" << stats.truncated_tail
              << " out_of_band=" << drops.price_out_of_band
              << " pool_exhausted=" << drops.pool_exhausted
              << " unknown=" << drops.unknown_order << " duplicate=" << drops.duplicate_order
              << " trades=" << trades << " trade_shares=" << trade_shares << "\n";
    assert(trades > 0);
    assert(stats.short_frames > 0);
    assert(stats.truncated_tail > 0);
    assert(drops.pool_exhausted > 0);
    std::cout << "[PASS] fuzz: no sanitizer report and invariants held\n";
    return 0;
}
