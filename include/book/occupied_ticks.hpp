#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace book {

// Bitset of price ticks that currently hold at least one order.
// Finding the next occupied tick is a word scan, not a walk of every
// empty price level. The empty-level walk was the measured p99 cost
// on a gapped book.
class OccupiedTicks {
public:
    explicit OccupiedTicks(size_t n_ticks)
        : n_ticks_(n_ticks), words_((n_ticks + 63) / 64) {}

    void set(int32_t tick) {
        const size_t index = static_cast<size_t>(tick);
        words_[index / 64] |= uint64_t{1} << (index % 64);
    }

    void clear(int32_t tick) {
        const size_t index = static_cast<size_t>(tick);
        words_[index / 64] &= ~(uint64_t{1} << (index % 64));
    }

    // Greatest occupied tick strictly below `tick`, or -1.
    int32_t next_below(int32_t tick) const {
        if (tick <= 0) return -1;
        int32_t cursor = tick - 1;
        size_t word = static_cast<size_t>(cursor) / 64;
        const unsigned bit = static_cast<unsigned>(cursor) % 64;
        uint64_t mask = (bit == 63) ? ~uint64_t{0} : ((uint64_t{1} << (bit + 1)) - 1);
        while (true) {
            const uint64_t bits = words_[word] & mask;
            if (bits != 0) {
                const unsigned highest = 63u - static_cast<unsigned>(__builtin_clzll(bits));
                return static_cast<int32_t>(word * 64 + highest);
            }
            if (word == 0) return -1;
            --word;
            mask = ~uint64_t{0};
        }
    }

    // Least occupied tick strictly above `tick`, or -1.
    int32_t next_above(int32_t tick) const {
        const uint64_t start = static_cast<uint64_t>(tick) + 1;
        if (start >= n_ticks_) return -1;
        size_t word = static_cast<size_t>(start / 64);
        const unsigned bit = static_cast<unsigned>(start % 64);
        uint64_t mask = ~uint64_t{0} << bit;
        while (word < words_.size()) {
            const uint64_t bits = words_[word] & mask;
            if (bits != 0) {
                const unsigned lowest = static_cast<unsigned>(__builtin_ctzll(bits));
                const size_t found = word * 64 + lowest;
                if (found >= n_ticks_) return -1;
                return static_cast<int32_t>(found);
            }
            ++word;
            mask = ~uint64_t{0};
        }
        return -1;
    }

private:
    size_t n_ticks_;
    std::vector<uint64_t> words_;
};

} // namespace book
