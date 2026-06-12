#pragma once
#include "hft_simulator/matching.hpp"
#include <random>
#include <vector>
#include <string>

namespace hft {

class BotSimulator {
public:
    BotSimulator(MatchingEngine& eng, const std::string& symbol, double tick_size);
    ~BotSimulator();

    void step(double mid, double spread_ticks);

private:
    MatchingEngine& eng_;
    std::string symbol_;
    double tick_size_;
    std::mt19937 rng_;

    std::vector<uint64_t> active_limit_orders_;
};

}
