#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "order_pool.hpp"

namespace book {

// Rough heap cost of one resting order: the pool slot plus an
// unordered_map node and its bucket pointer. Ladders are not included.
constexpr uint64_t kBytesPerOrderEstimate = sizeof(Order) + 48 + 8;

// Pass-1 upper bound on live orders per locate: the running peak of adds
// minus deletes. Full executions and cancels-to-zero are ignored, so this
// overcounts, which is the safe direction for sizing a pool.
class LiveOrderPeak {
public:
    explicit LiveOrderPeak(size_t max_locate = 65536) : live_(max_locate, 0), peak_(max_locate, 0) {}

    void on_add(uint16_t locate) {
        if (locate >= live_.size()) return;
        const uint32_t now = ++live_[locate];
        if (now > peak_[locate]) peak_[locate] = now;
    }

    void on_delete(uint16_t locate) {
        if (locate >= live_.size() || live_[locate] == 0) return;
        --live_[locate];
    }

    const std::vector<uint32_t>& peaks() const { return peak_; }

private:
    std::vector<uint32_t> live_;
    std::vector<uint32_t> peak_;
};

struct CapacityPlan {
    std::vector<uint32_t> capacity;  // per locate, 0 where peak is 0
    uint32_t cap = 0;                // water level; UINT32_MAX when nothing is cut
    uint64_t total_slots = 0;
    std::vector<uint16_t> cut_locates;  // peak > capacity
};

// Gives every locate its peak if the total fits in budget_slots. Otherwise
// finds the largest uniform cap c with sum(min(peak, c)) <= budget_slots and
// reports each locate whose peak exceeds c.
inline CapacityPlan plan_capacity(const std::vector<uint32_t>& peaks, uint64_t budget_slots) {
    CapacityPlan plan;
    plan.capacity.assign(peaks.size(), 0);

    uint64_t total = 0;
    for (const uint32_t p : peaks) total += p;

    uint32_t cap = UINT32_MAX;
    if (total > budget_slots) {
        std::vector<uint32_t> sorted;
        for (const uint32_t p : peaks) {
            if (p > 0) sorted.push_back(p);
        }
        std::sort(sorted.begin(), sorted.end());
        uint64_t below = 0;  // sum of peaks fully granted
        cap = 0;
        for (size_t i = 0; i < sorted.size(); ++i) {
            const uint64_t remaining = sorted.size() - i;
            // Every remaining locate gets the same cap; it cannot exceed sorted[i].
            const uint64_t level = (budget_slots - below) / remaining;
            if (level < sorted[i]) {
                cap = static_cast<uint32_t>(level);
                break;
            }
            below += sorted[i];
            cap = sorted[i];
        }
    }
    plan.cap = cap;

    for (size_t locate = 0; locate < peaks.size(); ++locate) {
        const uint32_t granted = std::min(peaks[locate], cap);
        plan.capacity[locate] = granted;
        plan.total_slots += granted;
        if (peaks[locate] > granted) plan.cut_locates.push_back(static_cast<uint16_t>(locate));
    }
    return plan;
}

} // namespace book
