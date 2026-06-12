#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <algorithm>
#include <limits>

namespace hft {

class LatencyHistogram {
public:
    static constexpr size_t MAGNITUDES   = 64;
    static constexpr size_t SUB_BITS     = 4;
    static constexpr size_t SUB_BUCKETS  = size_t(1) << SUB_BITS;
    static constexpr size_t TOTAL_BUCKETS = MAGNITUDES * SUB_BUCKETS;

    void record(uint64_t value_ns) {
        buckets_[bucket_index(value_ns)].fetch_add(1, std::memory_order_relaxed);
        total_count_.fetch_add(1, std::memory_order_relaxed);
        running_sum_.fetch_add(value_ns, std::memory_order_relaxed);
        update_min(value_ns);
        update_max(value_ns);
    }

    double percentile(double p) const {
        uint64_t total = total_count_.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        p = std::clamp(p, 0.0, 1.0);
        uint64_t target = static_cast<uint64_t>(p * total);
        if (target == 0) target = 1;
        uint64_t cum = 0;
        for (size_t i = 0; i < TOTAL_BUCKETS; ++i) {
            cum += buckets_[i].load(std::memory_order_relaxed);
            if (cum >= target) return static_cast<double>(bucket_to_ns(i));
        }
        return static_cast<double>(max_.load(std::memory_order_relaxed));
    }

    uint64_t count() const { return total_count_.load(std::memory_order_relaxed); }

    uint64_t min() const {
        uint64_t m = min_.load(std::memory_order_relaxed);
        return m == std::numeric_limits<uint64_t>::max() ? 0 : m;
    }

    uint64_t max() const { return max_.load(std::memory_order_relaxed); }

    double mean() const {
        uint64_t cnt = count();
        if (cnt == 0) return 0.0;
        return static_cast<double>(running_sum_.load(std::memory_order_relaxed)) / cnt;
    }

    void reset() {
        total_count_.store(0, std::memory_order_relaxed);
        running_sum_.store(0, std::memory_order_relaxed);
        min_.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
        max_.store(0, std::memory_order_relaxed);
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    }

private:

    size_t bucket_index(uint64_t value_ns) const {
        if (value_ns < SUB_BUCKETS) return value_ns;
        size_t magnitude = 63 - static_cast<size_t>(__builtin_clzll(value_ns));
        size_t shift = magnitude - SUB_BITS;
        size_t sub = (value_ns >> shift) & (SUB_BUCKETS - 1);
        return magnitude * SUB_BUCKETS + sub;
    }

    uint64_t bucket_to_ns(size_t index) const {
        size_t magnitude = index / SUB_BUCKETS;
        size_t sub       = index % SUB_BUCKETS;
        if (magnitude < SUB_BITS) return index;
        size_t shift = magnitude - SUB_BITS;
        return (uint64_t(SUB_BUCKETS) << shift) + (uint64_t(sub) << shift);
    }

    void update_min(uint64_t v) {
        uint64_t cur = min_.load(std::memory_order_relaxed);
        while (v < cur &&
               !min_.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    void update_max(uint64_t v) {
        uint64_t cur = max_.load(std::memory_order_relaxed);
        while (v > cur &&
               !max_.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    alignas(64) std::array<std::atomic<uint64_t>, TOTAL_BUCKETS> buckets_{};
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> running_sum_{0};
    std::atomic<uint64_t> min_{std::numeric_limits<uint64_t>::max()};
    std::atomic<uint64_t> max_{0};
};

}
