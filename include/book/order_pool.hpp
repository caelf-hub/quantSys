#pragma once

#include <cstdint>
#include <vector>

namespace book {

constexpr int32_t kNullIndex = -1;

// An order resting in the book. prev_/next_ are indices into the owning
// OrderPool's slot array, forming an intrusive doubly-linked list for
// the price level this order belongs to (FIFO time priority).
struct Order {
    uint64_t order_ref_number = 0;
    uint32_t price_raw = 0;   // fixed-point price, 4 implied decimals (ITCH convention)
    uint32_t shares = 0;
    char side = 0;            // 'B' or 'S'
    int32_t prev = kNullIndex;
    int32_t next = kNullIndex;
    bool in_use = false;
};

// Fixed-capacity pool allocator for Order slots. No malloc/new on the
// hot path after construction -- alloc()/free() just manipulate a
// singly-linked free list threaded through the same array via `next`.
class OrderPool {
public:
    explicit OrderPool(size_t capacity) : slots_(capacity) {
        for (size_t i = 0; i < capacity; ++i) {
            slots_[i].next = (i + 1 < capacity) ? static_cast<int32_t>(i + 1) : kNullIndex;
        }
        free_head_ = capacity > 0 ? 0 : kNullIndex;
    }

    int32_t alloc() {
        if (free_head_ == kNullIndex) return kNullIndex; // pool exhausted
        int32_t idx = free_head_;
        free_head_ = slots_[idx].next;
        slots_[idx] = Order{};
        slots_[idx].in_use = true;
        return idx;
    }

    void free(int32_t idx) {
        slots_[idx] = Order{};
        slots_[idx].next = free_head_;
        free_head_ = idx;
    }

    Order& operator[](int32_t idx) { return slots_[idx]; }
    const Order& operator[](int32_t idx) const { return slots_[idx]; }

    size_t capacity() const { return slots_.size(); }

private:
    std::vector<Order> slots_;
    int32_t free_head_ = kNullIndex;
};

} // namespace book
