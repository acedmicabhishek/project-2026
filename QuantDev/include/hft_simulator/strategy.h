#pragma once

#include "hft_simulator/market.h"
#include <memory>
#include <vector>

namespace hft {

struct Order {
    enum class Side { Buy, Sell };
    Side side;
    double price;
    int size;
};

class Strategy {
public:
    explicit Strategy(std::string name);
    virtual ~Strategy() = default;
    const std::string& name() const;
    virtual std::vector<Order> generate_orders(const Tick& market_state) = 0;

private:
    std::string name_;
};

class MomentumStrategy : public Strategy {
public:
    explicit MomentumStrategy(int lookback = 3);
    std::vector<Order> generate_orders(const Tick& market_state) override;

private:
    int lookback_;
    std::vector<double> history_;
};

class MeanReversionStrategy : public Strategy {
public:
    explicit MeanReversionStrategy(int window = 5);
    std::vector<Order> generate_orders(const Tick& market_state) override;

private:
    int window_;
    std::vector<double> history_;
};

}
