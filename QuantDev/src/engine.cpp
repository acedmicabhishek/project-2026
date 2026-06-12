#include "hft_simulator/engine.h"
#include "hft_simulator/benchmark.h"
#include <chrono>
#include <thread>

namespace hft {

SimulationEngine::SimulationEngine(OrderBook market, std::unique_ptr<Strategy> strategy, double latency_ms)
    : market_(std::move(market)), strategy_(std::move(strategy)), latency_ms_(latency_ms), order_count_(0), total_latency_seconds_(0.0) {}

SimulationSummary SimulationEngine::run(const std::vector<Tick>& market_data, int max_steps) {
    auto start_time = Clock::now();
    trades_.clear();
    order_count_ = 0;
    total_latency_seconds_ = 0.0;

    for (int i = 0; i < static_cast<int>(market_data.size()); ++i) {
        if (max_steps >= 0 && i >= max_steps) {
            break;
        }
        Tick market_state = market_.snapshot();
        market_state = market_data[i];
        auto step_start = Clock::now();
        std::vector<Order> orders = strategy_->generate_orders(market_state);
        if (latency_ms_ > 0.0) {
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(latency_ms_));
        }
        for (auto const& order : orders) {
            execute_order(order);
        }
        auto step_end = Clock::now();
        total_latency_seconds_ += seconds_elapsed(step_start, step_end);
    }

    auto end_time = Clock::now();
    double runtime = seconds_elapsed(start_time, end_time);
    double pnl = 0.0;
    for (auto const& trade : trades_) {
        pnl += (trade.side == Order::Side::Buy ? 1.0 : -1.0) * trade.price * trade.size;
    }

    return SimulationSummary{
        strategy_->name(),
        order_count_,
        static_cast<int>(trades_.size()),
        pnl,
        runtime,
        order_count_ > 0 ? total_latency_seconds_ / order_count_ : 0.0,
    };
}

const std::vector<Trade>& SimulationEngine::trades() const {
    return trades_;
}

void SimulationEngine::execute_order(const Order& order) {
    ++order_count_;
    double execution_price = order.price;
    market_.update_from_trade(execution_price);
    trades_.push_back(Trade{order.side, execution_price, order.size, 0.0});
}

}
