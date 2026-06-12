
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <deque>
#include <cmath>
#include <numeric>

namespace {
    static const int    BB_WINDOW   = 50;
    static const double BB_MULT     = 2.0;
    static const double POS_SIZE    = 40.0;
    static const double MAX_POS     = 160.0;

    std::deque<double> price_buf;
}

extern "C" const char* strategy_name() { return "Mean Reversion (BB50)"; }

extern "C" void on_init(int, double) { price_buf.clear(); }

extern "C" StrategyOrder on_tick(StrategyContext ctx) {
    if (ctx.mid <= 0.0) return { Action::None, 0, 0 };

    price_buf.push_back(ctx.mid);
    if ((int)price_buf.size() > BB_WINDOW) price_buf.pop_front();
    if ((int)price_buf.size() < BB_WINDOW / 2) return { Action::None, 0, 0 };

    double sum = 0; for (double p : price_buf) sum += p;
    double mean = sum / price_buf.size();
    double sq_sum = 0; for (double p : price_buf) sq_sum += (p-mean)*(p-mean);
    double stdev = std::sqrt(sq_sum / price_buf.size());

    double upper = mean + BB_MULT * stdev;
    double lower = mean - BB_MULT * stdev;
    double pos   = ctx.position;

    if (ctx.mid > upper && pos > -MAX_POS) {

        return { Action::Sell, POS_SIZE, ctx.bid };
    } else if (ctx.mid < lower && pos < MAX_POS) {

        return { Action::Buy, POS_SIZE, ctx.ask };
    } else if (std::abs(ctx.mid - mean) < stdev * 0.3 && pos != 0.0) {

        return { Action::Close, std::abs(pos), ctx.mid };
    }
    return { Action::None, 0, 0 };
}
