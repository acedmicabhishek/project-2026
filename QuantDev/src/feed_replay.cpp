#include "hft_simulator/feed.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <random>
#include <cmath>

namespace hft {

void CsvTickReplay::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("CsvTickReplay: cannot open " + path);

    ticks_.clear();
    std::string line;
    std::getline(f, line);

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        TickEvent t;
        char sep;
        std::string is_trade_str;

        ss >> t.ts_ns >> sep;
        std::getline(ss, t.symbol, ',');
        ss >> t.price >> sep >> t.bid >> sep >> t.ask >> sep >> t.size >> sep;
        std::getline(ss, is_trade_str);
        t.is_trade = (is_trade_str == "1" || is_trade_str == "true");

        ticks_.push_back(t);
    }
}

void CsvTickReplay::play(TickCallback cb, double speed_multiplier) {
    if (ticks_.empty()) return;

    using namespace std::chrono;
    using hrc = high_resolution_clock;

    if (speed_multiplier <= 0.0) {

        for (const auto& t : ticks_) cb(t);
        return;
    }

    uint64_t first_data_ts = ticks_.front().ts_ns;
    auto wall_start = hrc::now();

    for (const auto& t : ticks_) {

        double data_elapsed_ns = static_cast<double>(t.ts_ns - first_data_ts);
        double wall_elapsed_ns = data_elapsed_ns / speed_multiplier;

        auto target = wall_start + nanoseconds(static_cast<int64_t>(wall_elapsed_ns));
        auto now    = hrc::now();
        if (now < target)
            std::this_thread::sleep_for(target - now);

        cb(t);
    }
}

struct SyntheticFeed::Impl {
    std::mt19937                     rng;
    std::normal_distribution<double> dist{0.0, 1.0};
};

SyntheticFeed::SyntheticFeed(Params p)
    : params_(std::move(p)), price_(params_.initial_price),
      impl_(std::make_unique<Impl>())
{
    unsigned s = (params_.seed != 0) ? params_.seed
                                     : std::random_device{}();
    impl_->rng.seed(s);
}

TickEvent SyntheticFeed::next_tick() {

    double dW    = impl_->dist(impl_->rng);
    double log_r = (params_.mu - 0.5 * params_.sigma * params_.sigma) +
                    params_.sigma * dW;
    price_ = std::max(params_.tick_size, price_ * std::exp(log_r));

    price_ = std::round(price_ / params_.tick_size) * params_.tick_size;

    double half_spread = params_.spread_ticks * params_.tick_size / 2.0;

    TickEvent t;
    t.ts_ns    = ts_ns_ += 1'000'000;
    t.symbol   = params_.symbol;
    t.price    = price_;
    t.bid      = price_ - half_spread;
    t.ask      = price_ + half_spread;
    t.size     = 100;
    t.is_trade = false;
    return t;
}

std::vector<TickEvent> SyntheticFeed::generate(size_t n) {
    std::vector<TickEvent> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(next_tick());
    return out;
}

SyntheticFeed::~SyntheticFeed() = default;

std::vector<double> SyntheticFeed::generate_ou(size_t n, OUParams p, unsigned seed) {
    std::mt19937 rng(seed ? seed : std::random_device{}());
    std::normal_distribution<double> dist(0.0, 1.0);

    std::vector<double> out(n);
    double x = p.mu;
    for (size_t i = 0; i < n; ++i) {
        double dW = dist(rng);
        x += p.theta * (p.mu - x) + p.sigma * dW;
        out[i] = x;
    }
    return out;
}

struct OUSpreadFeed::Impl {
    std::mt19937                     rng;
    std::normal_distribution<double> dist{0.0, 1.0};
};

OUSpreadFeed::OUSpreadFeed(std::string sym_a, std::string sym_b,
                              double initial_price,
                              double theta, double mu_spread, double sigma_spread,
                              double sigma_price, double tick_size,
                              double spread_ticks, double dt, unsigned seed)
    : sym_a_(std::move(sym_a)), sym_b_(std::move(sym_b)),
      price_a_(initial_price), price_b_(initial_price), spread_(mu_spread),
      theta_(theta), mu_spread_(mu_spread), sigma_spread_(sigma_spread),
      sigma_price_(sigma_price), tick_size_(tick_size),
      half_spread_(spread_ticks * tick_size / 2.0), dt_(dt),
      impl_(std::make_unique<Impl>())
{
    unsigned s = seed ? seed : std::random_device{}();
    impl_->rng.seed(s);
}

std::pair<TickEvent, TickEvent> OUSpreadFeed::next() {
    auto& rng  = impl_->rng;
    auto& dist = impl_->dist;

    double dW1 = dist(rng), dW2 = dist(rng), dWs = dist(rng);

    double common = sigma_price_ * std::sqrt(dt_) * dW1;
    price_a_ = std::max(tick_size_, price_a_ * (1.0 + common + sigma_price_ * dW2 * 0.3));

    spread_ += theta_ * (mu_spread_ - spread_) * dt_ + sigma_spread_ * std::sqrt(dt_) * dWs;
    price_b_ = std::max(tick_size_, price_a_ + spread_);

    ts_ns_ += static_cast<uint64_t>(dt_ * 1'000'000);

    auto snap = [&](const std::string& sym, double price) -> TickEvent {
        double p = std::round(price / tick_size_) * tick_size_;
        TickEvent t;
        t.ts_ns  = ts_ns_;
        t.symbol = sym;
        t.price  = p;
        t.bid    = std::round((p - half_spread_) / tick_size_) * tick_size_;
        t.ask    = std::round((p + half_spread_) / tick_size_) * tick_size_;
        t.size   = 100;
        return t;
    };
    return {snap(sym_a_, price_a_), snap(sym_b_, price_b_)};
}

OUSpreadFeed::~OUSpreadFeed() = default;

}
