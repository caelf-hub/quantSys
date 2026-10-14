#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T should be trivially copyable for this benchmark");

public:
    bool try_push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_cached_) {
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if (next == tail_cached_) {
                return false; // full
            }
        }
        buffer_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> try_pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_cached_) {
            head_cached_ = head_.load(std::memory_order_acquire);
            if (tail == head_cached_) {
                return std::nullopt; // empty
            }
        }
        T value = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return value;
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::size_t tail_cached_{0};
    alignas(64) std::size_t head_cached_{0};

    alignas(64) T buffer_[Capacity];
};
