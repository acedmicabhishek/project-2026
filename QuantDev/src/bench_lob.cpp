
#include "hft_simulator/orderbook.hpp"
#include "hft_simulator/matching.hpp"
#include "hft_simulator/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace hft;
using Clock = std::chrono::high_resolution_clock;

static double to_ns(Clock::duration d) {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

struct Stats {
    double min_ns, max_ns, mean_ns;
    double p50_ns, p95_ns, p99_ns, p999_ns;
    double throughput_mops;
};

static Stats compute_stats(std::vector<double>& s) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    Stats r{};
    r.min_ns  = s.front();
    r.max_ns  = s.back();
    r.mean_ns = std::accumulate(s.begin(), s.end(), 0.0) / n;
    r.p50_ns  = s[n * 50 / 100];
    r.p95_ns  = s[n * 95 / 100];
    r.p99_ns  = s[n * 99 / 100];
    r.p999_ns = s[std::min(n * 999 / 1000, n - 1)];
    r.throughput_mops = 1e9 / r.mean_ns / 1e6;
    return r;
}

static void print_stats(const char* label, const Stats& s) {
    std::cout << "\n  " << label << "\n"
              << "  ┌─────────────────────────────────────────────────┐\n"
              << "  │  Throughput :  " << std::fixed << std::setprecision(2)
              << std::setw(8) << s.throughput_mops << " M ops/sec              │\n"
              << "  │  Latency    :  min=" << std::setw(5) << (int)s.min_ns
              << "ns   mean=" << std::setw(6) << (int)s.mean_ns << "ns        │\n"
              << "  │  Percentile :  p50=" << std::setw(5) << (int)s.p50_ns
              << "ns   p95=" << std::setw(6)  << (int)s.p95_ns  << "ns        │\n"
              << "  │               p99=" << std::setw(5) << (int)s.p99_ns
              << "ns   p999=" << std::setw(5) << (int)s.p999_ns << "ns        │\n"
              << "  └─────────────────────────────────────────────────┘\n";
}

static Order make_limit(uint64_t& seq, const std::string& sym,
                         Side side, double price, uint32_t qty = 100) {
    Order o;
    o.id = ++seq; o.symbol = sym; o.side = side;
    o.type = OrdType::Limit; o.price = price; o.qty = qty;
    o.ts_ns = seq * 1'000;
    return o;
}

static Order make_market(uint64_t& seq, const std::string& sym, Side side, uint32_t qty = 100) {
    Order o;
    o.id = ++seq; o.symbol = sym; o.side = side;
    o.type = OrdType::Market; o.qty = qty; o.price = 0.0;
    o.ts_ns = seq * 1'000;
    return o;
}

static Stats bench_add(size_t N, size_t warmup) {
    LimitOrderBook book("AAPL");
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> px(99.0, 101.0);
    std::uniform_int_distribution<uint32_t> qty(100, 1000);

    std::vector<Order> orders(N + warmup);
    uint64_t seq = 0;
    for (auto& o : orders) {
        o = make_limit(seq, "AAPL", seq % 2 == 0 ? Side::Buy : Side::Sell,
                       std::round(px(rng) * 100) / 100.0, qty(rng));
    }

    for (size_t i = 0; i < warmup; ++i) book.add(orders[i]);

    std::vector<double> samples(N);
    for (size_t i = 0; i < N; ++i) {
        auto t0 = Clock::now();
        book.add(orders[warmup + i]);
        samples[i] = to_ns(Clock::now() - t0);
    }
    return compute_stats(samples);
}

static Stats bench_cancel(size_t N, size_t warmup) {
    LimitOrderBook book("AAPL");
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> px(99.0, 101.0);

    std::vector<Order> orders(N + warmup);
    uint64_t seq = 0;
    for (auto& o : orders) {
        o = make_limit(seq, "AAPL", seq % 2 == 0 ? Side::Buy : Side::Sell,
                       std::round(px(rng) * 100) / 100.0);
        book.add(o);
    }

    std::vector<double> samples(N);
    for (size_t i = 0; i < N; ++i) {
        auto t0 = Clock::now();
        book.cancel(orders[warmup + i].id);
        samples[i] = to_ns(Clock::now() - t0);
    }
    return compute_stats(samples);
}

static Stats bench_match(size_t N, size_t warmup) {
    MatchingEngine eng;
    const std::string sym = "AAPL";
    uint64_t seq = 0;

    for (int i = 0; i < 100; ++i) {
        eng.submit(make_limit(seq, sym, Side::Buy,  100.00 - i * 0.01, 1000));
        eng.submit(make_limit(seq, sym, Side::Sell, 100.01 + i * 0.01, 1000));
    }

    auto reseed = [&](Side side) {
        double px = (side == Side::Sell) ? 100.01 : 99.99;
        eng.submit(make_limit(seq, sym, side, px, 1000));
    };

    for (size_t i = 0; i < warmup; ++i) {
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        eng.submit(make_market(seq, sym, s, 100));
        reseed(s == Side::Buy ? Side::Sell : Side::Buy);
    }

    std::vector<double> samples(N);
    for (size_t i = 0; i < N; ++i) {
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        reseed(s == Side::Buy ? Side::Sell : Side::Buy);
        auto mo = make_market(seq, sym, s, 100);
        auto t0 = Clock::now();
        eng.submit(mo);
        samples[i] = to_ns(Clock::now() - t0);
    }
    return compute_stats(samples);
}

static Stats bench_mixed(size_t N, size_t warmup) {
    MatchingEngine eng;
    const std::string sym = "AAPL";
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> px(99.50, 100.50);
    std::uniform_int_distribution<int> act(0, 9);
    uint64_t seq = 0;
    std::vector<uint64_t> live;
    live.reserve(50'000);

    for (int i = 0; i < 50; ++i) {
        auto b = make_limit(seq, sym, Side::Buy,  99.99 - i * 0.01, 500);
        auto a = make_limit(seq, sym, Side::Sell, 100.01 + i * 0.01, 500);
        eng.submit(b); live.push_back(b.id);
        eng.submit(a); live.push_back(a.id);
    }

    for (size_t i = 0; i < warmup; ++i)
        eng.submit(make_limit(seq, sym, Side::Buy, 99.95, 100));

    std::vector<double> samples(N);
    for (size_t i = 0; i < N; ++i) {
        int a = act(rng);
        auto t0 = Clock::now();
        if (a < 6) {
            Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            auto o = make_limit(seq, sym, s, std::round(px(rng) * 100) / 100.0);
            eng.submit(o);
            live.push_back(o.id);
        } else if (a < 8 && !live.empty()) {
            size_t idx = rng() % live.size();
            eng.cancel(live[idx]);
            live.erase(live.begin() + static_cast<ptrdiff_t>(idx));
        } else {
            Side s = (i % 3 == 0) ? Side::Buy : Side::Sell;
            eng.submit(make_market(seq, sym, s, 100));
        }
        samples[i] = to_ns(Clock::now() - t0);
    }
    return compute_stats(samples);
}

int main(int argc, char** argv) {
    size_t N      = 500'000;
    size_t warmup = 10'000;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--orders") && i + 1 < argc) N      = std::stoull(argv[++i]);
        if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = std::stoull(argv[++i]);
    }

    std::cout
        << "\n  ╔══════════════════════════════════════════════════════╗\n"
        << "  ║       QuantSim — LOB Matching Engine Benchmark      ║\n"
        << "  ╠══════════════════════════════════════════════════════╣\n"
        << "  ║  Ops per bench : " << std::setw(7) << N / 1000 << "k                           ║\n"
        << "  ║  Warmup ops    : " << std::setw(7) << warmup / 1000 << "k                           ║\n"
        << "  ║  Platform      : "
#if defined(__aarch64__)
        << "ARM64 / Apple Silicon                 ║\n"
#elif defined(__x86_64__)
        << "x86-64                                ║\n"
#else
        << "unknown                               ║\n"
#endif
        << "  ║  Optimisation  : "
#ifdef NDEBUG
        << "-O3 -march=native  (Release)          ║\n"
#else
        << "Debug (rebuild with -DCMAKE_BUILD_TYPE=Release) ║\n"
#endif
        << "  ╚══════════════════════════════════════════════════════╝\n";

    std::cout << "\n  Running 4 benchmarks ...\n";

    auto s1 = bench_add(N, warmup);
    print_stats("1. LimitOrderBook::add   (passive limit order insert)", s1);

    auto s2 = bench_cancel(N, warmup);
    print_stats("2. LimitOrderBook::cancel  (O(1) hash-map cancel)", s2);

    auto s3 = bench_match(N / 5, warmup / 5);
    print_stats("3. MatchingEngine::submit  (market order, single-level fill)", s3);

    auto s4 = bench_mixed(N, warmup);
    print_stats("4. Mixed workload  (60% add | 20% cancel | 20% market)", s4);

    std::cout
        << "\n  ┌──────────────────────────────────────────────────────────┐\n"
        << "  │                      SUMMARY                             │\n"
        << "  ├──────────────────────────────────────────────────────────┤\n"
        << "  │  Add throughput      " << std::setw(8) << s1.throughput_mops
        << " M orders/sec              │\n"
        << "  │  Cancel throughput   " << std::setw(8) << s2.throughput_mops
        << " M ops/sec                 │\n"
        << "  │  Match latency p50   " << std::setw(8) << (int)s3.p50_ns
        << " ns                        │\n"
        << "  │  Match latency p99   " << std::setw(8) << (int)s3.p99_ns
        << " ns                        │\n"
        << "  │  Mixed p99           " << std::setw(8) << (int)s4.p99_ns
        << " ns                        │\n"
        << "  └──────────────────────────────────────────────────────────┘\n";

    bool sub_us = (s3.p50_ns < 1000.0);
    std::cout << "\n  " << (sub_us ? "✓" : "✗")
              << "  Match p50 = " << (int)s3.p50_ns << " ns  ("
              << (sub_us ? "sub-microsecond ✓" : "need -O3 -march=native") << ")\n\n";

    return 0;
}
