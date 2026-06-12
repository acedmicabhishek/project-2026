#include "hft_simulator/market.h"
#include <random>

namespace hft {

OrderBook::OrderBook(double initial_price)
    : bid_(initial_price - 0.01), ask_(initial_price + 0.01), last_price_(initial_price) {}

void OrderBook::update_from_trade(double price) {
    last_price_ = price;
    bid_ = price - 0.01;
    ask_ = price + 0.01;
}

Tick OrderBook::snapshot() const {
    return Tick{0.0, last_price_, bid_, ask_};
}

MarketDataGenerator::MarketDataGenerator(double initial_price, double volatility, unsigned int seed)
    : price_(initial_price), volatility_(volatility), rng_(seed ? seed : std::random_device{}()) {}

Tick MarketDataGenerator::next_tick() {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    double move = price_ * volatility_ * dist(rng_);
    price_ = std::max(0.01, price_ + move);
    return Tick{0.0, price_, price_ - 0.01, price_ + 0.01};
}

std::vector<Tick> MarketDataGenerator::generate_series(int length) {
    std::vector<Tick> ticks;
    ticks.reserve(length);
    for (int i = 0; i < length; ++i) {
        ticks.push_back(next_tick());
    }
    return ticks;
}

}
