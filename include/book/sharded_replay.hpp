#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "locate_indexed_book.hpp"
#include "spsc_ring_buffer.hpp"

namespace book {

// One book event for a shard worker. Parser thread is the only producer.
enum class ShardOp : uint8_t { Add = 1, Cancel, Delete, Execute, Replace, Stop };

struct ShardCmd {
    ShardOp op = ShardOp::Stop;
    char side = 0;
    uint16_t locate = 0;
    uint32_t shares = 0;
    uint32_t price_raw = 0;
    uint64_t ref = 0;
    uint64_t ref2 = 0;
};

inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

// Parser thread owns the tape order. Each worker owns locates where
// locate % shard_count == shard_id, fed by a Phase-1 SPSC ring. Symbols
// are independent; the rings never share a cache line for head/tail.
class ShardedReplay {
public:
    static constexpr std::size_t kRing = 65536;

    ShardedReplay(std::size_t shard_count, uint32_t tick_size_raw, double band_pct,
                  std::size_t max_orders_per_symbol, std::size_t max_locate = 65536)
        : n_(shard_count == 0 ? 1 : shard_count) {
        shards_.reserve(n_);
        for (std::size_t i = 0; i < n_; ++i) {
            shards_.push_back(std::make_unique<Shard>(tick_size_raw, band_pct,
                                                      max_orders_per_symbol, max_locate));
        }
    }

    ShardedReplay(const ShardedReplay&) = delete;
    ShardedReplay& operator=(const ShardedReplay&) = delete;

    ~ShardedReplay() { stop(); }

    void set_capacities(std::vector<uint32_t> capacities) {
        for (auto& shard : shards_) shard->book.set_capacities(capacities);
    }

    void start() {
        if (started_) return;
        started_ = true;
        for (auto& shard : shards_) {
            Shard* s = shard.get();
            s->worker = std::thread([s] { s->run(); });
        }
    }

    void add_order(uint16_t locate, uint64_t ref, char side, uint32_t price_raw, uint32_t shares) {
        push(locate, ShardCmd{ShardOp::Add, side, locate, shares, price_raw, ref, 0});
    }
    void cancel_shares(uint16_t locate, uint64_t ref, uint32_t shares) {
        push(locate, ShardCmd{ShardOp::Cancel, 0, locate, shares, 0, ref, 0});
    }
    void delete_order(uint16_t locate, uint64_t ref) {
        push(locate, ShardCmd{ShardOp::Delete, 0, locate, 0, 0, ref, 0});
    }
    void execute_shares(uint16_t locate, uint64_t ref, uint32_t shares) {
        push(locate, ShardCmd{ShardOp::Execute, 0, locate, shares, 0, ref, 0});
    }
    void replace_order(uint16_t locate, uint64_t old_ref, uint64_t new_ref, uint32_t price_raw,
                       uint32_t shares) {
        push(locate, ShardCmd{ShardOp::Replace, 0, locate, shares, price_raw, old_ref, new_ref});
    }

    void stop() {
        if (!started_ || stopped_) return;
        stopped_ = true;
        for (std::size_t i = 0; i < n_; ++i) {
            ShardCmd end;
            end.op = ShardOp::Stop;
            while (!shards_[i]->ring.try_push(end)) cpu_pause();
        }
        for (auto& shard : shards_) {
            if (shard->worker.joinable()) shard->worker.join();
        }
    }

    std::size_t shard_count() const { return n_; }
    std::size_t shard_of(uint16_t locate) const { return static_cast<std::size_t>(locate) % n_; }

    const LocateIndexedBook& book(uint16_t locate) const {
        return shards_[shard_of(locate)]->book;
    }
    LocateIndexedBook& book(uint16_t locate) { return shards_[shard_of(locate)]->book; }

    uint64_t applied_events(uint16_t locate) const { return book(locate).applied_events(locate); }
    bool has_locate(uint16_t locate) const { return book(locate).has_locate(locate); }
    double best_bid(uint16_t locate) const { return book(locate).best_bid(locate); }
    double best_ask(uint16_t locate) const { return book(locate).best_ask(locate); }
    size_t resting_order_count(uint16_t locate) const {
        return book(locate).resting_order_count(locate);
    }
    DropCounts locate_drops(uint16_t locate) const { return book(locate).locate_drops(locate); }
    size_t pool_capacity(uint16_t locate) const { return book(locate).pool_capacity(locate); }

    DropCounts drop_counts() const {
        DropCounts total;
        for (const auto& shard : shards_) {
            const DropCounts d = shard->book.drop_counts();
            total.price_out_of_band += d.price_out_of_band;
            total.pool_exhausted += d.pool_exhausted;
            total.unknown_order += d.unknown_order;
            total.duplicate_order += d.duplicate_order;
        }
        return total;
    }

    uint64_t push_spins() const {
        uint64_t n = 0;
        for (const auto& shard : shards_) n += shard->push_spins.load(std::memory_order_relaxed);
        return n;
    }

private:
    struct Shard {
        SpscRingBuffer<ShardCmd, kRing> ring;
        LocateIndexedBook book;
        std::thread worker;
        std::atomic<uint64_t> push_spins{0};

        Shard(uint32_t tick, double band, size_t cap, size_t max_locate)
            : book(tick, band, cap, max_locate) {}

        void run() {
            for (;;) {
                auto cmd = ring.try_pop();
                if (!cmd) {
                    cpu_pause();
                    continue;
                }
                if (cmd->op == ShardOp::Stop) return;
                apply(*cmd);
            }
        }

        void apply(const ShardCmd& c) {
            switch (c.op) {
                case ShardOp::Add:
                    book.add_order(c.locate, c.ref, c.side, c.price_raw, c.shares);
                    break;
                case ShardOp::Cancel:
                    book.cancel_shares(c.locate, c.ref, c.shares);
                    break;
                case ShardOp::Delete:
                    book.delete_order(c.locate, c.ref);
                    break;
                case ShardOp::Execute:
                    book.execute_shares(c.locate, c.ref, c.shares);
                    break;
                case ShardOp::Replace:
                    book.replace_order(c.locate, c.ref, c.ref2, c.price_raw, c.shares);
                    break;
                case ShardOp::Stop:
                    break;
            }
        }
    };

    void push(uint16_t locate, const ShardCmd& cmd) {
        if (!started_) start();
        Shard& s = *shards_[shard_of(locate)];
        while (!s.ring.try_push(cmd)) {
            s.push_spins.fetch_add(1, std::memory_order_relaxed);
            cpu_pause();
        }
    }

    std::size_t n_;
    std::vector<std::unique_ptr<Shard>> shards_;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace book
