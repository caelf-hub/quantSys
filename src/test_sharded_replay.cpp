#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "book/locate_indexed_book.hpp"
#include "book/sharded_replay.hpp"
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

Tape make_tape() {
    Tape tape;
    tape.add(1, 1, 'B', 100, 1000000);
    tape.add(1, 2, 'B', 200, 999900);
    tape.add(1, 3, 'S', 300, 1000100);
    tape.add(1, 4, 'S', 50, 1000200);
    tape.execute(1, 1, 40);
    tape.cancel(1, 3, 100);
    tape.replace(1, 2, 5, 250, 1000000);
    tape.execute(1, 1, 60);
    tape.remove(1, 4);
    tape.add(1, 6, 'B', 10, 3000000);
    tape.add(1, 5, 'B', 10, 1000000);
    tape.cancel(1, 99, 1);
    tape.execute(2, 1, 1);
    tape.add(3, 7, 'S', 500, 50000);
    return tape;
}

struct Outcome {
    uint64_t applied = 0;
    DropCounts drops;
    double bid1 = 0;
    double ask1 = 0;
    double ask3 = 0;
    size_t resting1 = 0;
};

Outcome sequential(const Tape& tape) {
    LocateIndexedBook market(100, 0.5, 16, 8);
    Outcome o;
    ParserCallbacks cb;
    cb.on_add_order = [&](const AddOrder& m) {
        o.applied += market.add_order(m.header.stock_locate, m.order_ref_number,
                                      m.buy_sell_indicator, m.price_raw, m.shares);
    };
    cb.on_order_executed = [&](const OrderExecuted& m) {
        o.applied += market.execute_shares(m.header.stock_locate, m.order_ref_number,
                                           m.executed_shares);
    };
    cb.on_order_cancel = [&](const OrderCancel& m) {
        o.applied += market.cancel_shares(m.header.stock_locate, m.order_ref_number,
                                          m.cancelled_shares);
    };
    cb.on_order_delete = [&](const OrderDelete& m) {
        o.applied += market.delete_order(m.header.stock_locate, m.order_ref_number);
    };
    cb.on_order_replace = [&](const OrderReplace& m) {
        o.applied += market.replace_order(m.header.stock_locate, m.original_order_ref_number,
                                          m.new_order_ref_number, m.price_raw, m.shares);
    };
    process_buffer(tape.bytes().data(), tape.bytes().size(), cb);
    o.drops = market.drop_counts();
    o.bid1 = market.best_bid(1);
    o.ask1 = market.best_ask(1);
    o.ask3 = market.best_ask(3);
    o.resting1 = market.resting_order_count(1);
    return o;
}

Outcome sharded(const Tape& tape, std::size_t n) {
    ShardedReplay market(n, 100, 0.5, 16, 8);
    market.start();
    ParserCallbacks cb;
    cb.on_add_order = [&](const AddOrder& m) {
        market.add_order(m.header.stock_locate, m.order_ref_number, m.buy_sell_indicator,
                         m.price_raw, m.shares);
    };
    cb.on_order_executed = [&](const OrderExecuted& m) {
        market.execute_shares(m.header.stock_locate, m.order_ref_number, m.executed_shares);
    };
    cb.on_order_cancel = [&](const OrderCancel& m) {
        market.cancel_shares(m.header.stock_locate, m.order_ref_number, m.cancelled_shares);
    };
    cb.on_order_delete = [&](const OrderDelete& m) {
        market.delete_order(m.header.stock_locate, m.order_ref_number);
    };
    cb.on_order_replace = [&](const OrderReplace& m) {
        market.replace_order(m.header.stock_locate, m.original_order_ref_number,
                             m.new_order_ref_number, m.price_raw, m.shares);
    };
    process_buffer(tape.bytes().data(), tape.bytes().size(), cb);
    market.stop();
    Outcome o;
    o.applied = market.applied_events(1) + market.applied_events(2) + market.applied_events(3);
    o.drops = market.drop_counts();
    o.bid1 = market.best_bid(1);
    o.ask1 = market.best_ask(1);
    o.ask3 = market.best_ask(3);
    o.resting1 = market.resting_order_count(1);
    return o;
}

void same(const Outcome& a, const Outcome& b) {
    assert(a.applied == b.applied);
    assert(a.drops.price_out_of_band == b.drops.price_out_of_band);
    assert(a.drops.duplicate_order == b.drops.duplicate_order);
    assert(a.drops.unknown_order == b.drops.unknown_order);
    assert(a.drops.pool_exhausted == b.drops.pool_exhausted);
    assert(near(a.bid1, b.bid1));
    assert(near(a.ask1, b.ask1));
    assert(near(a.ask3, b.ask3));
    assert(a.resting1 == b.resting1);
}

} // namespace

int main() {
    const Tape tape = make_tape();
    const Outcome seq = sequential(tape);
    assert(seq.applied == 10);
    assert(seq.drops.price_out_of_band == 1);
    assert(seq.drops.duplicate_order == 1);
    assert(seq.drops.unknown_order == 2);

    same(seq, sharded(tape, 1));
    same(seq, sharded(tape, 2));
    same(seq, sharded(tape, 4));
    std::cout << "[PASS] 1/2/4 shards match sequential books and drop counts\n";

    ShardedReplay two(2, 100, 0.5, 16, 8);
    assert(two.shard_of(1) != two.shard_of(2));
    two.add_order(1, 1, 'B', 1000000, 100);
    two.add_order(2, 2, 'S', 1000100, 100);
    two.stop();
    assert(two.has_locate(1));
    assert(two.has_locate(2));
    assert(!two.book(1).has_locate(2));
    assert(!two.book(2).has_locate(1));
    std::cout << "[PASS] odd/even locates stay on disjoint books\n";
    return 0;
}
