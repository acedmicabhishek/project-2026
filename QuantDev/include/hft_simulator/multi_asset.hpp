#pragma once

#include "hft_simulator/feed.hpp"
#include <vector>
#include <string>
#include <random>
#include <cmath>

namespace hft {

class MultiAssetFeed {
public:
    MultiAssetFeed(std::vector<std::string> syms,
                   double sigma_common, double sigma_idio,
                   double initial_price = 100.0,
                   double tick_size = 0.01, double spread_ticks = 2.0,
                   unsigned seed = 0, double theta = 0.05)
        : syms_(std::move(syms)),
          sigma_common_(sigma_common), sigma_idio_(sigma_idio),
          tick_size_(tick_size), half_spread_(spread_ticks * tick_size * 0.5),
          init_price_(initial_price), theta_(theta),
          rng_(seed), norm_(0.0, 1.0) {
        const size_t n = syms_.size();
        price_.assign(n, initial_price);
        resid_.assign(n, 0.0);
        beta_.resize(n);
        std::uniform_real_distribution<double> bd(0.4, 1.6);
        for (size_t i = 0; i < n; ++i) beta_[i] = bd(rng_);
    }

    size_t size() const { return syms_.size(); }
    const std::vector<std::string>& symbols() const { return syms_; }

    std::vector<TickEvent> next() {
        base_log_ += sigma_common_ * norm_(rng_);
        std::vector<TickEvent> out(syms_.size());
        ts_ns_ += 1'000'000;
        for (size_t i = 0; i < syms_.size(); ++i) {

            resid_[i] = (1.0 - theta_) * resid_[i] + sigma_idio_ * norm_(rng_);
            price_[i] = init_price_ * std::exp(beta_[i] * base_log_ + resid_[i]);
            TickEvent t;
            t.ts_ns  = ts_ns_;
            t.symbol = syms_[i];
            t.price  = price_[i];
            t.bid    = price_[i] - half_spread_;
            t.ask    = price_[i] + half_spread_;
            t.size   = 0;
            t.is_trade = false;
            out[i] = std::move(t);
        }
        return out;
    }

private:
    std::vector<std::string> syms_;
    std::vector<double> price_, beta_, resid_;
    double sigma_common_, sigma_idio_, tick_size_, half_spread_, init_price_, theta_;
    double base_log_{0.0};
    uint64_t ts_ns_{0};
    std::mt19937 rng_;
    std::normal_distribution<double> norm_;
};

inline std::vector<std::vector<TickEvent>>
load_multi_csv(const std::vector<std::string>& paths) {
    std::vector<std::vector<TickEvent>> out;
    out.reserve(paths.size());
    for (const auto& p : paths) {
        CsvTickReplay r;
        r.load(p);
        out.push_back(r.ticks());
    }
    return out;
}

}
