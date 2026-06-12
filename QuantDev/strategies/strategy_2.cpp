
#include <hft_simulator/strategies/custom_strat_api.hpp>
#include <cstdio>

extern "C" const char* strategy_name() {
    return "Custom SMA Strategy";
}

extern "C" void on_init(int ticks, double initial_cash) {

}

extern "C" StrategyOrder on_tick(StrategyContext ctx) {

    static double last_mid = 0.0;
    StrategyOrder order = { Action::None, 0.0, 0.0 };
    if (last_mid > 0.0) {
        if (ctx.mid > last_mid && ctx.position < 100.0) {
            order = { Action::Buy, 10.0, ctx.ask };
        } else if (ctx.mid < last_mid && ctx.position > -100.0) {
            order = { Action::Sell, 10.0, ctx.bid };
        }
    }
    last_mid = ctx.mid;
    return order;
}
