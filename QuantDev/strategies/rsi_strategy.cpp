
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <cmath>

namespace {
    static const int    RSI_PERIOD  = 14;
    static const double OVERSOLD    = 30.0;
    static const double OVERBOUGHT  = 70.0;
    static const double POS_SIZE    = 30.0;
    static const double MAX_POS     = 120.0;

    std::deque<double> closes;
    double prev_close = 0.0;
}

static double compute_rsi(const std::deque<double>& c) {
    if (c.size() < 2) return 50.0;
    double gains = 0, losses = 0;
    for (size_t i = 1; i < c.size(); ++i) {
        double d = c[i] - c[i-1];
        if (d > 0) gains  += d;
        else       losses -= d;
    }
    int n = (int)c.size() - 1;
    if (n == 0) return 50.0;
    double avg_g = gains  / n;
    double avg_l = losses / n;
    if (avg_l == 0.0) return 100.0;
    double rs = avg_g / avg_l;
    return 100.0 - 100.0 / (1.0 + rs);
}

extern "C" const char* strategy_name() { return "RSI Reversal (14)"; }

extern "C" void on_init(int, double) { closes.clear(); prev_close = 0.0; }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };

    closes.push_back(ctx.mid);
    if ((int)closes.size() > RSI_PERIOD + 5) closes.pop_front();
    if ((int)closes.size() < RSI_PERIOD) return { Action::None, 0, 0 };

    double rsi = compute_rsi(closes);
    double pos = ctx.position;

    if (rsi < OVERSOLD && pos < MAX_POS) {
        return { Action::Buy, POS_SIZE, ctx.ask };
    } else if (rsi > OVERBOUGHT && pos > -MAX_POS) {
        return { Action::Sell, POS_SIZE, ctx.bid };
    } else if (rsi > 50.0 && rsi < 55.0 && pos < 0) {
        return { Action::Close, std::abs(pos), ctx.mid };
    } else if (rsi < 50.0 && rsi > 45.0 && pos > 0) {
        return { Action::Close, pos, ctx.mid };
    }
    return { Action::None, 0, 0 };
}
