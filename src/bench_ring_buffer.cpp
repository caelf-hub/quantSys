#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <x86intrin.h>
#include <pthread.h>
#include <sched.h>

#include "spsc_ring_buffer.hpp"
#include "spsc_ring_buffer_seqcst.hpp"

constexpr std::size_t kCapacity = 1 << 16;
constexpr std::size_t kOps = 10'000'000;

static void pin_to_core(std::thread& t, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Warning: failed to pin thread to core " << core_id
                  << " (errno " << rc << ")\n";
    }
}

static double cycles_per_ns() {
    using namespace std::chrono;
    unsigned aux;
    auto t0 = steady_clock::now();
    uint64_t c0 = __rdtscp(&aux);
    std::this_thread::sleep_for(milliseconds(200));
    auto t1 = steady_clock::now();
    uint64_t c1 = __rdtscp(&aux);
    double ns = static_cast<double>(duration_cast<nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns;
}

template <typename PushFn, typename PopFn>
void run_bench(const char* label, PushFn push, PopFn pop, double cyc_per_ns,
               int producer_core, int consumer_core) {
    std::vector<uint64_t> latencies(kOps);
    std::atomic<bool> start{false};

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) { _mm_pause(); }
        for (std::size_t i = 0; i < kOps; ++i) {
            while (!push(static_cast<int>(i))) { _mm_pause(); }
        }
    });
    if (producer_core >= 0) pin_to_core(producer, producer_core);

    std::thread consumer([&] {
        unsigned aux;
        start.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < kOps; ++i) {
            uint64_t t0 = __rdtscp(&aux);
            int value;
            while (!pop(value)) { _mm_pause(); }
            uint64_t t1 = __rdtscp(&aux);
            latencies[i] = t1 - t0;
        }
    });
    if (consumer_core >= 0) pin_to_core(consumer, consumer_core);

    producer.join();
    consumer.join();

    std::sort(latencies.begin(), latencies.end());
    auto to_ns = [&](uint64_t cyc) { return cyc / cyc_per_ns; };

    std::cout << label << "\n"
              << "  p50:   " << to_ns(latencies[kOps * 50 / 100]) << " ns\n"
              << "  p99:   " << to_ns(latencies[kOps * 99 / 100]) << " ns\n"
              << "  p99.9: " << to_ns(latencies[kOps * 999 / 1000]) << " ns\n"
              << "  max:   " << to_ns(latencies.back()) << " ns\n\n";
}

int main() {
    double cyc_per_ns = cycles_per_ns();
    std::cout << "Calibration: " << cyc_per_ns << " cycles/ns\n\n";

    unsigned hw_threads = std::thread::hardware_concurrency();
    std::cout << "hardware_concurrency: " << hw_threads << "\n\n";

    int producer_core = hw_threads >= 2 ? 0 : -1;
    int consumer_core = hw_threads >= 2 ? 1 : -1;

    {
        auto ring = std::make_unique<SpscRingBuffer<int, kCapacity>>();
        run_bench(
            "Lock-free SPSC (acquire/release, pinned, _mm_pause)",
            [&](int v) { return ring->try_push(v); },
            [&](int& out) {
                auto r = ring->try_pop();
                if (!r) return false;
                out = *r;
                return true;
            },
            cyc_per_ns, producer_core, consumer_core);
    }

    {
        auto ring = std::make_unique<SpscRingBufferSeqCst<int, kCapacity>>();
        run_bench(
            "Lock-free SPSC (seq_cst, pinned, _mm_pause)",
            [&](int v) { return ring->try_push(v); },
            [&](int& out) {
                auto r = ring->try_pop();
                if (!r) return false;
                out = *r;
                return true;
            },
            cyc_per_ns, producer_core, consumer_core);
    }

    {
        std::queue<int> q;
        std::mutex m;
        run_bench(
            "std::mutex + std::queue (pinned, _mm_pause)",
            [&](int v) {
                std::lock_guard<std::mutex> lock(m);
                q.push(v);
                return true;
            },
            [&](int& out) {
                std::lock_guard<std::mutex> lock(m);
                if (q.empty()) return false;
                out = q.front();
                q.pop();
                return true;
            },
            cyc_per_ns, producer_core, consumer_core);
    }

    return 0;
}
