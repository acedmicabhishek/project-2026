
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <cmath>
#include <cstdio>

namespace {

    static const int    LOOKBACK    = 30;
    static const double THRESHOLD   = 0.0003;
    static const double POSITION_SZ = 50.0;
    static const double MAX_POS     = 200.0;

    std::deque<double> prices;
    double position_shadow = 0.0;
}

extern "C" const char* strategy_name() { return "Momentum (30T)"; }

extern "C" void on_init(int, double) {
    prices.clear();
    position_shadow = 0.0;
}

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };

    prices.push_back(ctx.mid);
    if ((int)prices.size() > LOOKBACK + 1) prices.pop_front();
    if ((int)prices.size() < LOOKBACK) return { Action::None, 0, 0 };

    double old_price = prices.front();
    double ret = (ctx.mid - old_price) / old_price;

    position_shadow = ctx.position;

    if (ret > THRESHOLD && position_shadow < MAX_POS) {

        return { Action::Buy, POSITION_SZ, ctx.ask };
    } else if (ret < -THRESHOLD && position_shadow > -MAX_POS) {

        return { Action::Sell, POSITION_SZ, ctx.bid };
    }
    return { Action::None, 0, 0 };
}
