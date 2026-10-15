#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "research/fill_model.hpp"

using namespace research;

namespace {

constexpr uint32_t P = 1000000;   // $100.00
constexpr uint32_t TICK = 100;
constexpr uint64_t S = 1'000'000'000ULL;

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

} // namespace

int main() {
    FillParams params;

    {
        FillSimulator sim(params);
        sim.on_add(1, 'B', P, 100);
        sim.on_add(2, 'B', P, 200);
        const size_t id = sim.place(0, 10, 'B', P, 300);
        sim.on_execute(11, 1, 100);
        assert(sim.orders()[id].queue_ahead == 200);
        sim.on_execute(12, 2, 200);
        assert(sim.orders()[id].queue_ahead == 0);
        assert(sim.orders()[id].kind == FillKind::Open);  // nothing traded past us yet
        sim.on_add(3, 'B', P, 150);
        sim.on_execute(13, 3, 60);
        assert(sim.orders()[id].kind == FillKind::Open);  // 60 < our 100 shares
        sim.on_execute(14, 3, 40);
        assert(sim.orders()[id].kind == FillKind::Queue);
        assert(sim.orders()[id].done_ns == 14);
        assert(sim.orders()[id].time_to_fill_ns() == 4);
        assert(sim.orders()[id].queue_ahead_at_entry == 300);
        assert(sim.active_count() == 0);
        std::cout << "[PASS] fills only after the queue ahead drains and later volume trades\n";
    }

    {
        FillSimulator sim(params);
        sim.on_add(1, 'S', P + TICK, 500);
        const size_t id = sim.place(0, 0, 'S', P + TICK, 500);
        sim.on_add(2, 'S', P + TICK, 300);
        sim.on_cancel(1, 2, 300);
        sim.on_add(3, 'S', P + TICK, 300);
        sim.on_delete(2, 3);
        assert(sim.orders()[id].queue_ahead == 500);
        sim.on_touch(params.max_life_ns - 1, P, P + TICK);
        assert(sim.orders()[id].kind == FillKind::Open);
        sim.on_touch(params.max_life_ns, P, P + TICK);
        assert(sim.orders()[id].kind == FillKind::Expired);
        std::cout << "[PASS] cancels by later arrivals do not move us up; expiry at max life\n";
    }

    {
        FillSimulator sim(params);
        sim.on_add(1, 'B', P, 100);
        sim.on_add(2, 'B', P, 100);
        const size_t id = sim.place(0, 0, 'B', P, 200);
        sim.on_delete(1, 1);
        assert(sim.orders()[id].queue_ahead == 100);
        sim.on_cancel(2, 2, 30);
        assert(sim.orders()[id].queue_ahead == 70);
        // Replace at the same price: the order leaves the queue ahead and
        // rejoins behind us under a new reference.
        sim.on_replace(3, 2, 9, P, 70);
        assert(sim.orders()[id].queue_ahead == 0);
        sim.on_execute(4, 9, 100);
        assert(sim.orders()[id].kind == FillKind::Open);  // only 70 were resting
        std::cout << "[PASS] delete, cancel, and replace update the queue ahead correctly\n";
    }

    {
        FillSimulator sim(params);
        const size_t ask = sim.place(0, 0, 'S', P + TICK, 400);
        const size_t bid = sim.place(0, 0, 'B', P, 400);
        sim.on_touch(5, P + TICK, P + 2 * TICK);  // bid side reaches our ask
        assert(sim.orders()[ask].kind == FillKind::TradeThrough);
        assert(sim.orders()[bid].kind == FillKind::Open);
        sim.on_touch(6, P + TICK, P + 2 * TICK);  // bid touch 1 tick away: still open
        assert(sim.orders()[bid].kind == FillKind::Open);
        sim.on_touch(7, P + 2 * TICK, P + 3 * TICK);
        assert(sim.orders()[bid].kind == FillKind::Moved);
        std::cout << "[PASS] trade-through fill and touch-moved expiry\n";
    }

    {
        FillSimulator sim(params);
        const size_t bid = sim.place(0, 0, 'B', P, 0);
        const size_t ask = sim.place(0, 0, 'S', P + TICK, 0);
        sim.on_touch(1 * S, P - TICK, P);              // ask reaches our bid
        sim.on_touch(1 * S, P + TICK, P + 2 * TICK);   // bid reaches our ask
        assert(sim.orders()[bid].kind == FillKind::TradeThrough);
        assert(sim.orders()[ask].kind == FillKind::TradeThrough);
        const std::vector<uint64_t> ts = {1 * S, 2 * S, 6 * S};
        const std::vector<double> mid = {100.005, 100.02, 99.99};
        attach_markouts(sim.orders(), ts, mid);
        assert(near(sim.orders()[bid].markout[0], 0.02));     // mid up: bid gains
        assert(near(sim.orders()[ask].markout[0], -0.01));    // mid up: ask loses
        assert(near(sim.orders()[bid].markout[1], -0.01));
        assert(near(sim.orders()[ask].markout[1], 0.02));
        assert(std::isnan(sim.orders()[bid].markout[2]));     // past the data
        std::cout << "[PASS] markout sign per side and NaN past the data\n";
    }

    {
        FillSimulator sim(params);
        sim.place(0, 0, 'B', P, 10);
        sim.finish(99);
        assert(sim.orders()[0].kind == FillKind::End);
        assert(std::isnan(sim.orders()[0].markout[0]));
        std::cout << "[PASS] open orders end unfilled at the end of data\n";
    }

    std::cout << "All fill model tests passed.\n";
    return 0;
}
