#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

template <typename T, std::size_t Capacity>
class SpscRingBufferSeqCst {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T should be trivially copyable for this benchmark");

public:
    bool try_push(const T& value) noexcept {
        const std::size_t head = head_.load();
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_.load()) {
            return false;
        }
        buffer_[head] = value;
        head_.store(next);
        return true;
    }

    std::optional<T> try_pop() noexcept {
        const std::size_t tail = tail_.load();
        if (tail == head_.load()) {
            return std::nullopt;
        }
        T value = buffer_[tail];
        tail_.store((tail + 1) & mask_);
        return value;
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) T buffer_[Capacity];
};
