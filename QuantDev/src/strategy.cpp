#include "hft_simulator/strategy.h"

namespace hft {

Strategy::Strategy(std::string name) : name_(std::move(name)) {}

const std::string& Strategy::name() const {
    return name_;
}

MomentumStrategy::MomentumStrategy(int lookback)
    : Strategy("momentum"), lookback_(lookback) {}

std::vector<Order> MomentumStrategy::generate_orders(const Tick& market_state) {
    history_.push_back(market_state.price);
    if (static_cast<int>(history_.size()) <= lookback_) {
        return {};
    }
    int direction = history_.back() > history_[history_.size() - lookback_ - 1] ? 1 : -1;
    Order::Side side = direction > 0 ? Order::Side::Buy : Order::Side::Sell;
    double price = side == Order::Side::Buy ? market_state.ask : market_state.bid;
    return {{side, price, 1}};
}

MeanReversionStrategy::MeanReversionStrategy(int window)
    : Strategy("mean_reversion"), window_(window) {}

std::vector<Order> MeanReversionStrategy::generate_orders(const Tick& market_state) {
    history_.push_back(market_state.price);
    if (static_cast<int>(history_.size()) < window_) {
        return {};
    }
    double sum = 0.0;
    for (int i = static_cast<int>(history_.size()) - window_; i < static_cast<int>(history_.size()); ++i) {
        sum += history_[i];
    }
    double mean_price = sum / window_;
    if (market_state.price > mean_price * 1.001) {
        return {{Order::Side::Sell, market_state.bid, 1}};
    }
    if (market_state.price < mean_price * 0.999) {
        return {{Order::Side::Buy, market_state.ask, 1}};
    }
    return {};
}

}
