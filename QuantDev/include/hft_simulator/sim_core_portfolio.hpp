#pragma once

#include "hft_simulator/sim_core.hpp"
#include <memory>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace hft {

inline void SimRunner::run_portfolio(MatchingEngine& eng, int ticks, double,
                                     int seed, double cash, double&,
                                     double& cash_ref, bool enable_bots, bool csv_mode) {
    int N; double sc, si; std::string csv_multi;
    {
        std::lock_guard lk(state_.mu);
        N  = std::clamp(state_.pf_n_assets, 2, 10);
        sc = state_.pf_sigma_common; si = state_.pf_sigma_idio;
        csv_multi = state_.csv_paths_multi;
    }

    std::vector<std::string> syms;
    std::vector<std::vector<TickEvent>> csv_data;
    std::unique_ptr<MultiAssetFeed> feed;
    int total_ticks = ticks;
    bool use_csv = false;

    if (csv_mode && !csv_multi.empty()) {
        std::vector<std::string> paths;
        std::istringstream ss(csv_multi); std::string ln;
        while (std::getline(ss, ln)) {

            size_t a = ln.find_first_not_of(" \t\r\n");
            size_t b = ln.find_last_not_of(" \t\r\n");
            if (a != std::string::npos) paths.push_back(ln.substr(a, b - a + 1));
        }
        if ((int)paths.size() >= 2) {
            csv_data = load_multi_csv(paths);
            size_t minlen = SIZE_MAX;
            bool ok = true;
            for (auto& v : csv_data) { if (v.empty()) ok = false; minlen = std::min(minlen, v.size()); }
            if (ok && minlen > 0) {
                N = (int)csv_data.size();
                for (auto& v : csv_data) syms.push_back(v.front().symbol.empty() ? "ASSET" : v.front().symbol);
                total_ticks = (int)minlen;
                use_csv = true;
            }
        }
    }
    if (!use_csv) {
        for (int k = 0; k < N; ++k) syms.push_back("SYM" + std::to_string(k + 1));
        feed = std::make_unique<MultiAssetFeed>(syms, sc, si, 100.0, 0.01, 2.0, (unsigned)seed);
    }

    {
        std::lock_guard lk(state_.mu);
        state_.n_assets = N;
        state_.asset_names = syms;
        state_.asset_pos.assign(N, 0.0);
        state_.corr_matrix.assign(N * N, 0.0f);
        state_.total_ticks = total_ticks;
    }

    std::vector<std::unique_ptr<BotSimulator>> bots;
    bots.reserve(N);
    for (auto& s : syms) bots.push_back(std::make_unique<BotSimulator>(eng, s, 0.01));

    const int    W      = 30;
    const int    REB    = 40;
    const double ZMIN   = 0.7;
    const double GROSS  = cash * 0.5;
    std::vector<std::deque<double>> retbuf(N);
    std::vector<double> prev(N, 0.0), mid(N, 0.0);

    auto submit_market = [&](const std::string& sym, Side side, uint32_t qty) {
        if (qty == 0) return;
        Order o; o.id = eng.next_order_id(); o.symbol = sym; o.side = side;
        o.type = OrdType::Market; o.qty = qty;
        do_submit(eng, std::move(o));
    };

    for (int i = 0; i < total_ticks && !stop_flag_.load(); ++i) {

        if (use_csv) for (int k = 0; k < N; ++k) mid[k] = csv_data[k][i].mid();
        else {
            auto ticks_now = feed->next();
            for (int k = 0; k < N; ++k) mid[k] = ticks_now[k].mid();
        }
        cur_tick_ = i; cur_mid_ = mid[0];
        flush_pending(eng);

        if (enable_bots)
            for (int k = 0; k < N; ++k) bots[k]->step(mid[k], 2.0);

        for (int k = 0; k < N; ++k) {
            if (prev[k] > 0.0) {
                retbuf[k].push_back(std::log(mid[k] / prev[k]));
                if ((int)retbuf[k].size() > W) retbuf[k].pop_front();
            }
            prev[k] = mid[k];
        }

        bool windows_full = true;
        for (int k = 0; k < N; ++k) if ((int)retbuf[k].size() < W) windows_full = false;

        if (windows_full && (i % 10 == 0 || i == total_ticks - 1)) {
            std::vector<double> mean(N, 0.0), sd(N, 0.0);
            for (int k = 0; k < N; ++k) {
                for (double r : retbuf[k]) mean[k] += r;
                mean[k] /= W;
                for (double r : retbuf[k]) sd[k] += (r - mean[k]) * (r - mean[k]);
                sd[k] = std::sqrt(sd[k]);
            }
            std::vector<float> cm(N * N, 0.0f);
            for (int a = 0; a < N; ++a)
                for (int b = 0; b < N; ++b) {
                    if (a == b) { cm[a * N + b] = 1.0f; continue; }
                    double cov = 0.0;
                    for (int t = 0; t < W; ++t)
                        cov += (retbuf[a][t] - mean[a]) * (retbuf[b][t] - mean[b]);
                    double den = sd[a] * sd[b];
                    cm[a * N + b] = (float)(den > 1e-12 ? cov / den : 0.0);
                }
            std::lock_guard lk(state_.mu);
            state_.corr_matrix = std::move(cm);
        }

        if (windows_full && (i % REB == 0)) {

            std::vector<double> sig(N, 0.0), dev(N, 0.0);
            double msig = 0.0;
            for (int k = 0; k < N; ++k) { for (double r : retbuf[k]) sig[k] += r; msig += sig[k]; }
            msig /= N;
            double norm = 0.0, vsum = 0.0;
            for (int k = 0; k < N; ++k) { dev[k] = sig[k] - msig; norm += std::abs(dev[k]); vsum += dev[k]*dev[k]; }
            double dsd = std::sqrt(vsum / N);
            if (norm > 1e-9 && dsd > 1e-12) {
                for (int k = 0; k < N; ++k) {
                    double z = dev[k] / dsd;
                    if (std::abs(z) < ZMIN) continue;
                    double tgt_dollar = -GROSS * dev[k] / norm;
                    double tgt_shares = mid[k] > 0 ? tgt_dollar / mid[k] : 0.0;
                    double delta = tgt_shares - strat_pos_[syms[k]];
                    if (std::abs(delta) * mid[k] < 100.0) continue;
                    Side side = delta > 0 ? Side::Buy : Side::Sell;
                    submit_market(syms[k], side, (uint32_t)std::round(std::abs(delta)));
                }
            }
        }

        double eq = cash_ref;
        double bot_eq = cash + bot_cash_;
        std::vector<double> cur_pos(N);
        for (int k = 0; k < N; ++k) {
            cur_pos[k] = strat_pos_[syms[k]];
            eq += cur_pos[k] * mid[k];
            bot_eq += bot_positions_[syms[k]] * mid[k];
        }
        {
            std::lock_guard lk(state_.mu);
            state_.asset_pos = cur_pos;
        }
        state_.push_equity(eq, bot_eq);
        state_.tick_num = i + 1;
        if ((i % 50) == 0) state_.compute_metrics();
        if (state_.delay_us > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(state_.delay_us));
    }
}

}
