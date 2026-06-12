#pragma once

#include <random>
#include <vector>

namespace hft {

struct Tick {
    double timestamp;
    double price;
    double bid;
    double ask;
};

class OrderBook {
public:
    explicit OrderBook(double initial_price = 100.0);
    void update_from_trade(double price);
    Tick snapshot() const;

private:
    double bid_;
    double ask_;
    double last_price_;
};

class MarketDataGenerator {
public:
    MarketDataGenerator(double initial_price = 100.0, double volatility = 0.0005, unsigned int seed = 0);
    Tick next_tick();
    std::vector<Tick> generate_series(int length);

private:
    double price_;
    double volatility_;
    std::mt19937 rng_;
};

}
