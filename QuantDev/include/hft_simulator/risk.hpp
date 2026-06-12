#pragma once

#include "hft_simulator/order.hpp"
#include <atomic>
#include <chrono>
#include <string>
#include <cmath>

namespace hft {

class KillSwitch {
public:
    void arm()   { active_.store(true,  std::memory_order_release); }
    void disarm(){ active_.store(false, std::memory_order_release); }
    bool is_active() const { return active_.load(std::memory_order_acquire); }

    bool check() const noexcept { return active_.load(std::memory_order_relaxed); }

private:
    alignas(64) std::atomic<bool> active_{false};
};

class TokenBucket {
public:
    TokenBucket(double rate_per_sec, double burst)
        : rate_(rate_per_sec), tokens_(burst), max_(burst),
          last_ts_(clock::now()) {}

    bool allow() {
        auto now    = clock::now();
        double secs = std::chrono::duration<double>(now - last_ts_).count();
        last_ts_    = now;
        tokens_     = std::min(max_, tokens_ + rate_ * secs);
        if (tokens_ >= 1.0) { tokens_ -= 1.0; return true; }
        return false;
    }

    void reset() { tokens_ = max_; last_ts_ = clock::now(); }

private:
    using clock = std::chrono::steady_clock;
    double rate_, tokens_, max_;
    std::chrono::steady_clock::time_point last_ts_;
};

struct PreTradeRiskLimits {
    uint32_t max_order_qty         = 10'000;
    int64_t  max_position_notional = 1'000'000'00LL;
    double   max_price_dev_bps     = 50.0;
    double   order_rate_per_sec    = 500.0;
    double   burst                 = 50.0;
};

class PreTradeRisk {
public:
    using Limits = PreTradeRiskLimits;

    enum class Result {
        PASS,
        REJECT_KILL_SWITCH,
        REJECT_QTY,
        REJECT_NOTIONAL,
        REJECT_RATE,
        REJECT_PRICE,
    };

    static const char* result_str(Result r) {
        switch (r) {
            case Result::PASS:               return "PASS";
            case Result::REJECT_KILL_SWITCH: return "KILL_SWITCH";
            case Result::REJECT_QTY:         return "QTY_LIMIT";
            case Result::REJECT_NOTIONAL:    return "NOTIONAL_LIMIT";
            case Result::REJECT_RATE:        return "RATE_LIMIT";
            case Result::REJECT_PRICE:       return "PRICE_SANITY";
            default:                         return "UNKNOWN";
        }
    }

    PreTradeRisk(KillSwitch& ks, Limits limits = {})
        : kill_switch_(ks), limits_(limits),
          rate_limiter_(limits.order_rate_per_sec, limits.burst) {}

    Result check(const Order& order, double mid_price, int64_t current_pos_notional) {
        if (kill_switch_.check())
            return Result::REJECT_KILL_SWITCH;

        if (order.qty > limits_.max_order_qty)
            return Result::REJECT_QTY;

        double notional_cents = order.qty * order.price * 100.0;
        if (std::abs(static_cast<double>(current_pos_notional)) + notional_cents
            > static_cast<double>(limits_.max_position_notional))
            return Result::REJECT_NOTIONAL;

        if (!rate_limiter_.allow())
            return Result::REJECT_RATE;

        if (mid_price > 0.0 && order.type == OrdType::Limit) {
            double dev_bps = std::abs(order.price - mid_price) / mid_price * 10000.0;
            if (dev_bps > limits_.max_price_dev_bps)
                return Result::REJECT_PRICE;
        }

        return Result::PASS;
    }

    const Limits& limits() const { return limits_; }
    void set_limits(Limits l) { limits_ = l; rate_limiter_.reset(); }

private:
    KillSwitch&  kill_switch_;
    Limits       limits_;
    TokenBucket  rate_limiter_;
};

class StalePriceGuard {
public:
    explicit StalePriceGuard(int64_t max_age_ns, KillSwitch& ks)
        : max_age_ns_(max_age_ns), kill_switch_(ks) {}

    void on_tick(int64_t ts_ns) {
        last_tick_ns_.store(ts_ns, std::memory_order_release);
    }

    void check(int64_t now_ns) {
        int64_t last = last_tick_ns_.load(std::memory_order_acquire);
        if (last > 0 && (now_ns - last) > max_age_ns_)
            kill_switch_.arm();
    }

private:
    int64_t                  max_age_ns_;
    std::atomic<int64_t>     last_tick_ns_{0};
    KillSwitch&              kill_switch_;
};

}
