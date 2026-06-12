#include "hft_simulator/bot_sim.hpp"
#include <cmath>

namespace hft {

BotSimulator::BotSimulator(MatchingEngine& eng, const std::string& symbol, double tick_size)
    : eng_(eng), symbol_(symbol), tick_size_(tick_size)
{
    std::random_device rd;
    rng_.seed(rd());
}

BotSimulator::~BotSimulator() {
    for (uint64_t oid : active_limit_orders_) {
        eng_.cancel(oid);
    }
}

void BotSimulator::step(double mid, double spread_ticks) {

    for (uint64_t oid : active_limit_orders_) {
        eng_.cancel(oid);
    }
    active_limit_orders_.clear();

    double half_spread = (spread_ticks * tick_size_) / 2.0;
    double best_bid = mid - half_spread;
    double best_ask = mid + half_spread;

    std::uniform_int_distribution<uint32_t> qty_dist(50, 300);

    for (int i = 0; i < 5; ++i) {
        double bid_px = best_bid - i * tick_size_;
        double ask_px = best_ask + i * tick_size_;

        bid_px = std::round(bid_px / tick_size_) * tick_size_;
        ask_px = std::round(ask_px / tick_size_) * tick_size_;

        if (bid_px > 0) {
            Order bid_ord;
            bid_ord.id = eng_.next_order_id();
            bid_ord.symbol = symbol_;
            bid_ord.side = Side::Buy;
            bid_ord.type = OrdType::Limit;
            bid_ord.price = bid_px;
            bid_ord.qty = qty_dist(rng_);
            bid_ord.is_bot = true;
            eng_.submit(bid_ord);
            active_limit_orders_.push_back(bid_ord.id);
        }

        Order ask_ord;
        ask_ord.id = eng_.next_order_id();
        ask_ord.symbol = symbol_;
        ask_ord.side = Side::Sell;
        ask_ord.type = OrdType::Limit;
        ask_ord.price = ask_px;
        ask_ord.qty = qty_dist(rng_);
        ask_ord.is_bot = true;
        eng_.submit(ask_ord);
        active_limit_orders_.push_back(ask_ord.id);
    }

    std::uniform_real_distribution<double> prob_dist(0.0, 1.0);

    if (prob_dist(rng_) < 0.35) {
        Side taker_side = (prob_dist(rng_) < 0.5) ? Side::Buy : Side::Sell;
        std::uniform_int_distribution<uint32_t> taker_qty_dist(20, 150);

        Order market_ord;
        market_ord.id = eng_.next_order_id();
        market_ord.symbol = symbol_;
        market_ord.side = taker_side;
        market_ord.type = OrdType::Market;
        market_ord.qty = taker_qty_dist(rng_);
        market_ord.is_bot = true;
        eng_.submit(market_ord);
    }
}

}
