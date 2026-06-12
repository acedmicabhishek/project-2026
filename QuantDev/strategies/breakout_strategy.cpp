
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <algorithm>
#include <cmath>

namespace {
    static const int    CHANNEL_PERIOD = 20;
    static const double POS_SIZE       = 25.0;
    static const double MAX_POS        = 100.0;
    static const double STOP_LOSS_PCT  = 0.005;

    std::deque<double> highs, lows;
    double entry_price = 0.0;
    int    position_dir = 0;
}

extern "C" const char* strategy_name() { return "Breakout (Donchian 20)"; }

extern "C" void on_init(int, double) {
    highs.clear(); lows.clear();
    entry_price = 0.0; position_dir = 0;
}

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };

    highs.push_back(ctx.ask);
    lows.push_back(ctx.bid);
    if ((int)highs.size() > CHANNEL_PERIOD) { highs.pop_front(); lows.pop_front(); }
    if ((int)highs.size() < CHANNEL_PERIOD) return { Action::None, 0, 0 };

    double chan_high = *std::max_element(highs.begin(), highs.end());
    double chan_low  = *std::min_element(lows.begin(),  lows.end());
    double pos       = ctx.position;

    if (position_dir == 1 && entry_price > 0 &&
        ctx.mid < entry_price * (1.0 - STOP_LOSS_PCT)) {
        position_dir = 0; entry_price = 0.0;
        return { Action::Close, std::abs(pos), ctx.bid };
    }
    if (position_dir == -1 && entry_price > 0 &&
        ctx.mid > entry_price * (1.0 + STOP_LOSS_PCT)) {
        position_dir = 0; entry_price = 0.0;
        return { Action::Close, std::abs(pos), ctx.ask };
    }

    if (ctx.ask >= chan_high && pos < MAX_POS && position_dir != 1) {
        position_dir = 1; entry_price = ctx.ask;
        return { Action::Buy, POS_SIZE, ctx.ask };
    } else if (ctx.bid <= chan_low && pos > -MAX_POS && position_dir != -1) {
        position_dir = -1; entry_price = ctx.bid;
        return { Action::Sell, POS_SIZE, ctx.bid };
    }
    return { Action::None, 0, 0 };
}
