#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <numeric>
#include <algorithm>

namespace hft {

struct alignas(64) TradingMetrics {
    std::atomic<uint64_t> orders_sent{0};
    std::atomic<uint64_t> orders_filled{0};
    std::atomic<uint64_t> orders_cancelled{0};
    std::atomic<uint64_t> orders_rejected{0};
    std::atomic<int64_t>  net_position{0};
    std::atomic<int64_t>  gross_pnl_cents{0};

    static constexpr size_t HIST_BUCKETS = 1000;
    std::atomic<uint64_t> latency_hist[HIST_BUCKETS]{};
    std::atomic<uint64_t> latency_overflow{0};

    void record_order_sent()    { orders_sent.fetch_add(1,    std::memory_order_relaxed); }
    void record_order_rejected(){ orders_rejected.fetch_add(1,std::memory_order_relaxed); }
    void record_cancel()        { orders_cancelled.fetch_add(1,std::memory_order_relaxed); }

    void record_fill(char side, int qty, int64_t pnl_cents) {
        orders_filled.fetch_add(1, std::memory_order_relaxed);
        int64_t delta = (side == 'B') ? qty : -qty;
        net_position.fetch_add(delta, std::memory_order_relaxed);
        gross_pnl_cents.fetch_add(pnl_cents, std::memory_order_relaxed);
    }

    void record_latency_us(uint64_t us) {
        if (us < HIST_BUCKETS)
            latency_hist[us].fetch_add(1, std::memory_order_relaxed);
        else
            latency_overflow.fetch_add(1, std::memory_order_relaxed);
    }

    double percentile(double p) const {
        uint64_t total = 0;
        for (size_t i = 0; i < HIST_BUCKETS; ++i)
            total += latency_hist[i].load(std::memory_order_relaxed);
        total += latency_overflow.load(std::memory_order_relaxed);

        if (total == 0) return 0.0;
        uint64_t target = static_cast<uint64_t>(total * p);
        uint64_t cum = 0;
        for (size_t i = 0; i < HIST_BUCKETS; ++i) {
            cum += latency_hist[i].load(std::memory_order_relaxed);
            if (cum >= target) return static_cast<double>(i);
        }
        return static_cast<double>(HIST_BUCKETS);
    }

    void reset() {
        orders_sent.store(0, std::memory_order_relaxed);
        orders_filled.store(0, std::memory_order_relaxed);
        orders_cancelled.store(0, std::memory_order_relaxed);
        orders_rejected.store(0, std::memory_order_relaxed);
        net_position.store(0, std::memory_order_relaxed);
        gross_pnl_cents.store(0, std::memory_order_relaxed);
        for (auto& b : latency_hist) b.store(0, std::memory_order_relaxed);
        latency_overflow.store(0, std::memory_order_relaxed);
    }
};

inline uint64_t rdtsc() {
#ifdef __x86_64__
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

}
