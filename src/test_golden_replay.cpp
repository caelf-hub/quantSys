// End-to-end replay of a scripted tape: frame walk, parser, and book.
// The expected final state is worked out by hand in the comments.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "book/locate_indexed_book.hpp"
#include "itch/parser.hpp"

using namespace itch;
using namespace book;

namespace {

class Tape {
public:
    void add(uint16_t locate, uint64_t ref, char side, uint32_t shares, uint32_t price) {
        auto f = frame('A', 36, locate);
        f[19] = static_cast<uint8_t>(side);
        put(f, 11, ref, 8);
        put(f, 20, shares, 4);
        put(f, 32, price, 4);
        push(f);
    }
    void execute(uint16_t locate, uint64_t ref, uint32_t shares) {
        auto f = frame('E', 31, locate);
        put(f, 11, ref, 8);
        put(f, 19, shares, 4);
        push(f);
    }
    void cancel(uint16_t locate, uint64_t ref, uint32_t shares) {
        auto f = frame('X', 23, locate);
        put(f, 11, ref, 8);
        put(f, 19, shares, 4);
        push(f);
    }
    void remove(uint16_t locate, uint64_t ref) {
        auto f = frame('D', 19, locate);
        put(f, 11, ref, 8);
        push(f);
    }
    void replace(uint16_t locate, uint64_t old_ref, uint64_t new_ref, uint32_t shares,
                 uint32_t price) {
        auto f = frame('U', 35, locate);
        put(f, 11, old_ref, 8);
        put(f, 19, new_ref, 8);
        put(f, 27, shares, 4);
        put(f, 31, price, 4);
        push(f);
    }
    void directory(uint16_t locate, const char (&stock)[9]) {
        auto f = frame('R', 39, locate);
        for (size_t i = 0; i < 8; ++i) f[11 + i] = static_cast<uint8_t>(stock[i]);
        push(f);
    }
    void short_add(uint16_t locate) { push(frame('A', 20, locate)); }
    // A length prefix that promises more bytes than the tape holds.
    void truncated_tail() {
        bytes_.push_back(0);
        bytes_.push_back(36);
        bytes_.push_back(static_cast<uint8_t>('A'));
    }

    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;

    static std::vector<uint8_t> frame(char type, size_t len, uint16_t locate) {
        std::vector<uint8_t> f(len, 0);
        f[0] = static_cast<uint8_t>(type);
        put(f, 1, locate, 2);
        return f;
    }
    static void put(std::vector<uint8_t>& f, size_t offset, uint64_t value, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) {
            f[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(value & 0xFF);
            value >>= 8;
        }
    }
    void push(const std::vector<uint8_t>& f) {
        bytes_.push_back(static_cast<uint8_t>(f.size() >> 8));
        bytes_.push_back(static_cast<uint8_t>(f.size() & 0xFF));
        bytes_.insert(bytes_.end(), f.begin(), f.end());
    }
};

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

} // namespace

int main() {
    Tape tape;
    tape.directory(1, "TEST1   ");
    tape.directory(3, "TEST3   ");
    // Locate 1, $100.00 first price, band +/-50%.
    tape.add(1, 1, 'B', 100, 1000000);  // bid 100 @ 100.00
    tape.add(1, 2, 'B', 200, 999900);   // bid 200 @ 99.99
    tape.add(1, 3, 'S', 300, 1000100);  // ask 300 @ 100.01
    tape.add(1, 4, 'S', 50, 1000200);   // ask 50 @ 100.02
    tape.execute(1, 1, 40);             // ref 1 -> 60
    tape.cancel(1, 3, 100);             // ref 3 -> 200
    tape.replace(1, 2, 5, 250, 1000000);// ref 2 gone, ref 5 bid 250 @ 100.00
    tape.execute(1, 1, 60);             // ref 1 fully filled, removed
    tape.remove(1, 4);                  // ask @ 100.02 gone
    tape.add(1, 6, 'B', 10, 3000000);   // $300 is outside the band: out_of_band
    tape.add(1, 5, 'B', 10, 1000000);   // ref 5 already live: duplicate
    tape.cancel(1, 99, 1);              // never added: unknown
    tape.execute(2, 1, 1);              // locate 2 has no book: unknown
    tape.short_add(1);                  // short frame, not parsed
    tape.add(3, 7, 'S', 500, 50000);    // locate 3, ask 500 @ 5.00
    tape.truncated_tail();              // counted, stops the walk

    LocateIndexedBook market(/*tick=*/100, /*band_pct=*/0.5, /*max_orders_per_symbol=*/16, 8);
    uint64_t applied = 0;
    std::vector<uint16_t> directory_locates;
    ParserCallbacks cb;
    cb.on_stock_directory = [&](const StockDirectory& m) {
        assert(m.stock[0] == 'T' && m.stock[4] == static_cast<char>('0' + m.header.stock_locate));
        directory_locates.push_back(m.header.stock_locate);
    };
    cb.on_add_order = [&](const AddOrder& m) {
        applied += market.add_order(m.header.stock_locate, m.order_ref_number,
                                    m.buy_sell_indicator, m.price_raw, m.shares);
    };
    cb.on_order_executed = [&](const OrderExecuted& m) {
        applied += market.execute_shares(m.header.stock_locate, m.order_ref_number,
                                         m.executed_shares);
    };
    cb.on_order_cancel = [&](const OrderCancel& m) {
        applied += market.cancel_shares(m.header.stock_locate, m.order_ref_number,
                                        m.cancelled_shares);
    };
    cb.on_order_delete = [&](const OrderDelete& m) {
        applied += market.delete_order(m.header.stock_locate, m.order_ref_number);
    };
    cb.on_order_replace = [&](const OrderReplace& m) {
        applied += market.replace_order(m.header.stock_locate, m.original_order_ref_number,
                                        m.new_order_ref_number, m.price_raw, m.shares);
    };

    FrameStats stats;
    const size_t frames = process_buffer(tape.bytes().data(), tape.bytes().size(), cb, &stats);

    assert(frames == 17);
    assert(stats.short_frames == 1);
    assert(stats.truncated_tail == 1);
    assert((directory_locates == std::vector<uint16_t>{1, 3}));
    assert(applied == 10);

    assert(near(market.best_bid(1), 100.00));
    assert(near(market.best_ask(1), 100.01));
    assert(market.resting_order_count(1) == 2);  // ref 5 bid, ref 3 ask
    assert(near(market.best_ask(3), 5.00));
    assert(market.best_bid(3) == -1.0);

    const DropCounts drops = market.drop_counts();
    assert(drops.price_out_of_band == 1);
    assert(drops.duplicate_order == 1);
    assert(drops.unknown_order == 2);
    assert(drops.pool_exhausted == 0);

    std::cout << "frames=" << frames << " applied=" << applied << " short=" << stats.short_frames
              << " out_of_band=" << drops.price_out_of_band
              << " duplicate=" << drops.duplicate_order << " unknown=" << drops.unknown_order
              << "\n";
    std::cout << "[PASS] golden synthetic replay matches the hand-worked final state\n";
    return 0;
}
