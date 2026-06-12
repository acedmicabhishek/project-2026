#pragma once

#include "hft_simulator/market.h"
#include "hft_simulator/strategy.h"
#include <string>
#include <vector>

namespace hft {

struct Trade {
    Order::Side side;
    double price;
    int size;
    double timestamp;
};

struct SimulationSummary {
    std::string strategy;
    int orders_executed;
    int trade_count;
    double pnl;
    double runtime_seconds;
    double average_latency_seconds;
};

class SimulationEngine {
public:
    SimulationEngine(OrderBook market, std::unique_ptr<Strategy> strategy, double latency_ms = 0.0);
    SimulationSummary run(const std::vector<Tick>& market_data, int max_steps = -1);
    const std::vector<Trade>& trades() const;

private:
    void execute_order(const Order& order);

    OrderBook market_;
    std::unique_ptr<Strategy> strategy_;
    double latency_ms_;
    std::vector<Trade> trades_;
    int order_count_;
    double total_latency_seconds_;
};

}
