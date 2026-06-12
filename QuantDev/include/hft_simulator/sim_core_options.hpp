#pragma once

#include "hft_simulator/sim_core.hpp"
#include "hft_simulator/options.hpp"
#include <algorithm>
#include <cmath>

namespace hft {

inline void SimRunner::run_options(MatchingEngine& eng, int ticks, double sigma,
                                   int seed, double cash, double&,
                                   double& cash_ref, bool enable_bots, bool) {
    double tenor, iv, rate; bool hedge;
    {
        std::lock_guard lk(state_.mu);
        tenor = state_.opt_tenor > 0 ? state_.opt_tenor : 0.25;
        iv    = state_.opt_iv > 0 ? state_.opt_iv : 0.20;
        rate  = state_.opt_rate;
        hedge = state_.opt_hedge;
    }

    const std::string UND = "UND", OPT = "OPT";
    SyntheticFeed feed({UND, 100.0, 0.0, sigma, 0.01, 2.0, (unsigned)seed});
    BotSimulator und_bots(eng, UND, 0.01);
    BotSimulator opt_bots(eng, OPT, 0.01);

    const double K = 100.0;
    const int    reprice_int = 5;
    const int    hedge_int   = 3;
    uint64_t bid_id = 0, ask_id = 0;
    {
        std::lock_guard lk(state_.mu);
        state_.opt_strike = K; state_.total_ticks = ticks;
    }

    for (int i = 0; i < ticks && !stop_flag_.load(); ++i) {
        TickEvent ut = feed.next_tick();
        double S = ut.mid();
        cur_tick_ = i; cur_mid_ = S;
        flush_pending(eng);

        double T = std::max(1e-4, tenor * (1.0 - (double)i / ticks));
        Greeks g = bs_greeks(S, K, T, rate, iv,true);
        double theo = g.price;

        if (enable_bots) { und_bots.step(S, 2.0); opt_bots.step(theo, 2.0); }

        if (i % reprice_int == 0) {
            do_cancel(eng, bid_id); do_cancel(eng, ask_id);
            double edge = std::max(0.02, theo * 0.02);
            auto quote = [&](Side side, double px) -> uint64_t {
                Order o; o.id = eng.next_order_id(); o.symbol = OPT; o.side = side;
                o.type = OrdType::PostOnly; o.price = std::round(px * 100) / 100.0; o.qty = 5;
                return do_submit(eng, std::move(o));
            };
            bid_id = quote(Side::Buy,  theo - edge);
            ask_id = quote(Side::Sell, theo + edge);
        }

        double opt_pos = strat_pos_[OPT];
        double und_pos = strat_pos_[UND];

        if (hedge && i % hedge_int == 0) {
            double target_und = -opt_pos * g.delta;
            double d = target_und - und_pos;
            if (std::abs(d) >= 1.0) {
                Order o; o.id = eng.next_order_id(); o.symbol = UND;
                o.side = d > 0 ? Side::Buy : Side::Sell;
                o.type = OrdType::Market; o.qty = (uint32_t)std::round(std::abs(d));
                do_submit(eng, std::move(o));
                und_pos = strat_pos_[UND];
            }
        }

        double net_delta = opt_pos * g.delta + und_pos;
        double net_gamma = opt_pos * g.gamma;
        double net_vega  = opt_pos * g.vega;
        double net_theta = opt_pos * g.theta;

        if ((i % 10) == 0) {
            double strikes[5] = { S*0.96, S*0.98, S, S*1.02, S*1.04 };
            auto surf = bs_greeks_surface(S, std::span<const double>(strikes, 5),
                                          T, rate, iv,true);
            std::lock_guard lk(state_.mu);
            state_.greeks_surface = std::move(surf);
            state_.greeks_strikes.assign(strikes, strikes + 5);
        }

        double eq = cash_ref + opt_pos * theo + und_pos * S;
        {
            std::lock_guard lk(state_.mu);
            state_.opt_underlying = S; state_.opt_theo = theo; state_.opt_iv_live = iv;
            state_.opt_position = opt_pos; state_.opt_hedge_pos = und_pos;
            state_.net_delta = net_delta; state_.net_gamma = net_gamma;
            state_.net_vega = net_vega;   state_.net_theta = net_theta;
            state_.delta_series.push_back(net_delta);
        }
        state_.push_equity(eq);
        state_.tick_num = i + 1;
        if ((i % 50) == 0) state_.compute_metrics();
        if (state_.delay_us > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(state_.delay_us));
    }
}

}
