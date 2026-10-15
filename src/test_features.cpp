#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "research/features.hpp"

using namespace research;

namespace {

constexpr uint64_t MS = 1'000'000ULL;
constexpr uint64_t S = 1'000'000'000ULL;

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

struct Event {
    uint64_t ts;
    bool is_trade;
    TouchState touch;
    char side;
    uint32_t shares;
};

struct Snapshot {
    int64_t ofi;
    int64_t signed_volume;
    bool operator==(const Snapshot& o) const {
        return ofi == o.ofi && signed_volume == o.signed_volume;
    }
};

// Replays events in order and reads the tracker at each sample time, the
// same way the study does: a sample is read once the tape reaches it.
std::vector<Snapshot> replay(const std::vector<Event>& events,
                             const std::vector<uint64_t>& samples) {
    FeatureTracker tracker;
    std::vector<Snapshot> out;
    size_t next = 0;
    for (const Event& e : events) {
        while (next < samples.size() && samples[next] < e.ts) {
            out.push_back({tracker.ofi_1s(samples[next]), tracker.signed_volume_1s(samples[next])});
            ++next;
        }
        if (e.is_trade) {
            tracker.on_execution(e.ts, e.side, e.shares);
        } else {
            tracker.on_touch(e.ts, e.touch);
        }
    }
    while (next < samples.size()) {
        out.push_back({tracker.ofi_1s(samples[next]), tracker.signed_volume_1s(samples[next])});
        ++next;
    }
    return out;
}

} // namespace

int main() {
    {
        RollingSum sum;
        sum.add(0, 5);
        sum.add(500 * MS, 3);
        assert(sum.sum(900 * MS) == 8);
        assert(sum.sum(1 * S) == 3);  // the t=0 entry leaves at exactly 1s
        assert(sum.sum(1600 * MS) == 0);
        std::cout << "[PASS] rolling 1s sum over (now - 1s, now]\n";
    }

    {
        const TouchState base{1000000, 1000100, 100, 200};
        TouchState bigger_bid = base;
        bigger_bid.bid_shares = 150;
        assert(ofi_increment(base, bigger_bid) == 50);
        const TouchState bid_up{1000100, 1000200, 30, 200};
        assert(ofi_increment(base, bid_up) == 30 + 200);  // bid steps up, ask steps up
        const TouchState ask_down{1000000, 1000000 + 0, 100, 40};
        assert(ofi_increment(base, ask_down) == -40);     // new lower ask adds sell pressure
        TouchState bid_down = base;
        bid_down.bid_raw = 999900;
        bid_down.bid_shares = 70;
        assert(ofi_increment(base, bid_down) == -100);    // the old best bid is gone
        std::cout << "[PASS] OFI increments match Cont-Kukanov-Stoikov by hand\n";
    }

    {
        FeatureTracker tracker;
        const TouchState a{1000000, 1000100, 100, 200};
        TouchState b = a;
        b.bid_shares = 160;
        tracker.on_touch(0, a);
        tracker.on_touch(10 * MS, a);         // unchanged: no increment
        tracker.on_touch(20 * MS, b);         // +60
        tracker.on_touch(30 * MS, TouchState{0, 1000100, 0, 200});  // one-sided: skipped
        tracker.on_touch(40 * MS, b);         // previous was one-sided: skipped
        assert(tracker.ofi_1s(50 * MS) == 60);
        tracker.on_execution(60 * MS, 'B', 300);  // resting bid hit: sell
        tracker.on_execution(70 * MS, 'S', 100);  // resting ask lifted: buy
        tracker.on_execution(80 * MS, '?', 999);  // unknown side ignored
        assert(tracker.signed_volume_1s(90 * MS) == -200);
        assert(tracker.signed_volume_1s(1070 * MS) == 0);
        std::cout << "[PASS] tracker skips unchanged and one-sided touches; trades are signed\n";
    }

    assert(near(depth_imbalance(300, 100), 0.5));
    assert(near(depth_imbalance(0, 0), 0.0));
    assert(near(minutes_since(34'200'000'000'000ULL + 90 * S, 34'200'000'000'000ULL), 1.5));
    std::cout << "[PASS] depth imbalance and minutes since open\n";

    {
        std::vector<FeatureRow> rows(4);
        const uint64_t ts[] = {0, 500 * MS, 1200 * MS, 6000 * MS};
        const double mids[] = {10.0, 11.0, 13.0, 12.5};
        for (int i = 0; i < 4; ++i) {
            rows[static_cast<size_t>(i)].timestamp_ns = ts[i];
            rows[static_cast<size_t>(i)].mid = mids[i];
        }
        assert(near(lagged_mid_change(rows, 2, 1 * S), 3.0));
        assert(near(lagged_mid_change(rows, 1, 1 * S), 0.0));
        const double before = lagged_mid_change(rows, 2, 1 * S);
        rows.push_back(FeatureRow{});
        rows.back().timestamp_ns = 7 * S;
        rows.back().mid = 99.0;
        assert(near(lagged_mid_change(rows, 2, 1 * S), before));  // later rows unused
        attach_targets(rows);
        assert(near(rows[0].target[0], 13.0 - 10.0));  // first row at or after 1s
        assert(near(rows[0].target[1], 12.5 - 10.0));  // first row at or after 5s
        assert(std::isnan(rows[0].target[2]));         // data ends before 30s
        std::cout << "[PASS] lagged mid uses only earlier rows; targets look forward only\n";
    }

    {
        std::vector<Event> events;
        const TouchState a{1000000, 1000100, 100, 200};
        TouchState b = a;
        b.ask_shares = 120;
        events.push_back({0, false, a, 0, 0});
        events.push_back({100 * MS, false, b, 0, 0});
        events.push_back({200 * MS, true, {}, 'B', 50});
        const std::vector<uint64_t> samples = {150 * MS, 250 * MS};
        const auto base = replay(events, samples);
        std::vector<Event> longer = events;
        TouchState c = b;
        c.bid_shares = 900;
        longer.push_back({260 * MS, false, c, 0, 0});
        longer.push_back({300 * MS, true, {}, 'S', 777});
        const auto extended = replay(longer, samples);
        assert(base == extended);
        assert(base[1].ofi == 80 && base[1].signed_volume == -50);
        std::cout << "[PASS] events after the sample time leave its features unchanged\n";
    }

    std::cout << "All feature tests passed.\n";
    return 0;
}
