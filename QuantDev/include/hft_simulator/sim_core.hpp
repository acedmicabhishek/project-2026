#pragma once

#include "hft_simulator/order.hpp"
#include "hft_simulator/orderbook.hpp"
#include "hft_simulator/matching.hpp"
#include "hft_simulator/feed.hpp"
#include "hft_simulator/risk.hpp"
#include "hft_simulator/metrics.hpp"
#include "hft_simulator/bot_sim.hpp"

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <string>
#include <functional>
#include <cmath>
#include <cstdint>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <random>
#include <dlfcn.h>
#include "hft_simulator/strategies/custom_strat_api.hpp"
#include "hft_simulator/strategies/strategy_params.hpp"
#include "hft_simulator/multi_asset.hpp"

namespace hft {

struct SimState {
    std::mutex mu;

    double best_bid{0}, best_ask{0};
    uint32_t bid_sz{0}, ask_sz{0};
    std::vector<DepthLevel> bids, asks;

    struct FillRow {
        uint64_t id; std::string sym; std::string side;
        double qty; double price; uint64_t ts_ns; bool maker{false};
    };
    std::deque<FillRow> fill_log;

    struct BotFillRow {
        uint64_t id; std::string sym; std::string side;
        double qty; double price; uint64_t ts_ns;
    };
    std::deque<BotFillRow> bot_fill_log;

    std::vector<std::pair<double,double>> live_bids, live_asks;
    bool   live_mode{false};

    double pos_net{0}, pos_avg{0}, pos_realized{0}, pos_unreal{0};
    double feed_lat_us{0};
    std::string risk_state{"OK"}, risk_detail;
    uint64_t orders_blocked{0};
    double   maker_ratio{0};

    std::vector<double> equity;
    std::vector<double> bot_equity;
    double initial_cash{500'000.0};
    double final_pnl{0};

    double sharpe{0}, sortino{0}, calmar{0}, max_dd{0}, vol{0};
    uint64_t fills_total{0}, orders_sent{0};
    int64_t  net_position{0};
    uint64_t bot_fills_total{0};
    double   bot_volume{0.0};

    uint64_t win_trades{0};
    uint64_t loss_trades{0};
    double   win_ratio{0.0};
    double   profit_factor{1.0};
    double   gross_wins{0.0};
    double   gross_losses{0.0};

    std::unordered_map<std::string, ParamMap> params;
    double mm_gamma{150.0};
    double mm_kappa{1.5};

    ParamMap param_snapshot(const std::string& strat) {
        std::lock_guard lk(mu);
        ParamMap m = params.count(strat) ? params[strat] : ParamMap{};
        fill_defaults(strat, m);
        return m;
    }

    bool   latency_enabled{false};
    double latency_min_ms{2.0};
    double latency_max_ms{50.0};
    double avg_slippage{0.0};
    double avg_queue_pos{0.0};
    uint64_t slip_samples{0};

    bool   fees_enabled{false};
    double taker_fee_bps{1.0};
    double maker_rebate_bps{0.2};
    double borrow_bps_per_tick{0.0};
    double total_fees{0.0}, total_rebates{0.0}, total_borrow{0.0};
    uint64_t maker_fills{0}, taker_fills{0};

    bool   risk_enabled{false};
    double max_position{1000.0};
    double max_notional{0.0};
    double max_loss{0.0};
    bool   risk_halted{false};

    double var95{0.0};
    double gross_exposure{0.0}, net_exposure{0.0};
    double realized_pnl{0.0}, unrealized_pnl{0.0};

    int n_assets{0};
    std::vector<std::string>  asset_names;
    std::vector<float>        corr_matrix;
    std::vector<double>       asset_pos;

    int    pf_n_assets{4};
    double pf_sigma_common{0.0006};
    double pf_sigma_idio{0.0004};

    double opt_tenor{0.25};
    double opt_iv{0.20};
    double opt_rate{0.02};
    bool   opt_hedge{true};

    double opt_strike{0}, opt_theo{0}, opt_underlying{0}, opt_iv_live{0};
    double opt_position{0}, opt_hedge_pos{0};
    double net_delta{0}, net_gamma{0}, net_vega{0}, net_theta{0};
    std::vector<double> delta_series;

    std::vector<double> greeks_surface;
    std::vector<double> greeks_strikes;

    std::string csv_path{"trades.csv"};
    std::string csv_paths_multi;

    std::vector<double> correlation;
    double sa_spy_pos{0.0};
    double sa_qqq_pos{0.0};

    uint64_t tick_num{0};
    uint64_t total_ticks{0};
    bool     running{false};
    bool     done{false};
    bool     kill_armed{false};
    std::string strategy_name;
    int      delay_us{0};

    std::vector<double> prev_equity;
    std::vector<double> prev_bot_equity;
    double prev_sharpe{0}, prev_sortino{0}, prev_calmar{0}, prev_max_dd{0}, prev_vol{0};
    double prev_final_pnl{0}, prev_initial_cash{0};
    uint64_t prev_fills_total{0}, prev_orders_sent{0};
    uint64_t prev_bot_fills_total{0};
    double   prev_bot_volume{0.0};
    uint64_t prev_win_trades{0};
    uint64_t prev_loss_trades{0};
    double   prev_win_ratio{0.0};
    double   prev_profit_factor{1.0};
    std::string prev_strategy_name;
    bool has_prev{false};

    struct CompareSlot {
        std::vector<double> equity;
        double sharpe{0}, sortino{0}, calmar{0}, max_dd{0}, vol{0};
        double final_pnl{0}, initial_cash{0};
        uint64_t fills_total{0}, win_trades{0}, loss_trades{0};
        double win_ratio{0.0}, profit_factor{1.0};
        std::string strategy_name;
        bool valid{false};
    };
    CompareSlot cmp_slots[5];

    std::string custom_dylib_path;

    void save_to_compare(int slot_idx) {
        if (slot_idx < 0 || slot_idx >= 5) return;
        std::lock_guard lk(mu);
        auto& s = cmp_slots[slot_idx];
        s.equity         = equity;
        s.sharpe         = sharpe;   s.sortino  = sortino;
        s.calmar         = calmar;   s.max_dd   = max_dd;
        s.vol            = vol;      s.final_pnl= final_pnl;
        s.initial_cash   = initial_cash;
        s.fills_total    = fills_total;
        s.win_trades     = win_trades; s.loss_trades = loss_trades;
        s.win_ratio      = win_ratio; s.profit_factor = profit_factor;
        s.strategy_name  = strategy_name;
        s.valid          = true;
    }

    void push_fill(FillRow r) {
        std::lock_guard lk(mu);
        if (fill_log.size() >= 200) fill_log.pop_front();
        fill_log.push_back(std::move(r));
        ++fills_total;
    }

    void set_book(double bb, double ba, uint32_t bs, uint32_t as_,
                  std::vector<DepthLevel> b, std::vector<DepthLevel> a) {
        std::lock_guard lk(mu);
        best_bid=bb; best_ask=ba; bid_sz=bs; ask_sz=as_;
        bids=std::move(b); asks=std::move(a);
    }

    void set_live_book(double bb, double ba,
                       std::vector<std::pair<double,double>> b,
                       std::vector<std::pair<double,double>> a) {
        std::lock_guard lk(mu);
        best_bid=bb; best_ask=ba;
        live_bids=std::move(b); live_asks=std::move(a);
        _push_live_heatmap_column_locked();
    }

    void push_equity(double v) {
        std::lock_guard lk(mu);
        equity.push_back(v);
        bot_equity.push_back(bot_equity.empty() ? initial_cash : bot_equity.back());
    }

    void push_equity(double v, double bot_v) {
        std::lock_guard lk(mu);
        equity.push_back(v);
        bot_equity.push_back(bot_v);
    }

    void compute_metrics() {
        std::lock_guard lk(mu);
        win_ratio = (win_trades + loss_trades) > 0 ? (double)win_trades / (win_trades + loss_trades) : 0.0;
        profit_factor = gross_losses > 0.0 ? gross_wins / gross_losses : (gross_wins > 0.0 ? 99.9 : 1.0);
        realized_pnl = gross_wins - gross_losses;

        if (equity.size() < 2) return;
        auto& eq = equity;
        std::vector<double> rets;
        rets.reserve(eq.size());
        for (size_t i = 1; i < eq.size(); ++i) {
            double denom = eq[i-1] ? eq[i-1] : 1.0;
            rets.push_back((eq[i]-eq[i-1])/denom);
        }
        double mean = 0;
        for (double r : rets) mean += r;
        mean /= rets.size();

        double var = 0;
        for (double r : rets) var += (r-mean)*(r-mean);
        var /= rets.size();
        double sd = std::sqrt(var);
        if (sd < 1e-12) { sharpe=sortino=calmar=vol=0; return; }

        double ann = std::sqrt(252.0);
        sharpe  = mean / sd * ann;
        vol     = sd * ann;

        double down_var = 0; int dcount = 0;
        for (double r : rets) if (r < 0) { down_var += r*r; ++dcount; }
        double down_sd = dcount>0 ? std::sqrt(down_var/dcount)*ann : 1e-9;
        sortino = mean / (down_sd/ann) * ann;

        double peak = eq[0], dd = 0;
        for (double v : eq) {
            if (v > peak) peak = v;
            double d = (peak - v) / peak;
            if (d > dd) dd = d;
        }
        max_dd  = dd;
        double total_ret = (eq.back() - initial_cash) / initial_cash;
        calmar  = dd > 1e-9 ? total_ret / dd : 0;
        final_pnl = eq.back() - initial_cash;
        unrealized_pnl = final_pnl - realized_pnl;

        if (rets.size() >= 20) {
            std::vector<double> sr = rets;
            std::sort(sr.begin(), sr.end());
            var95 = -sr[(size_t)(0.05 * sr.size())] * eq.back();
        }
    }

    void save_to_prev() {
        std::lock_guard lk(mu);
        prev_equity      = equity;
        prev_bot_equity  = bot_equity;
        prev_sharpe      = sharpe;
        prev_sortino     = sortino;
        prev_calmar      = calmar;
        prev_max_dd      = max_dd;
        prev_vol         = vol;
        prev_final_pnl   = final_pnl;
        prev_initial_cash= initial_cash;
        prev_fills_total = fills_total;
        prev_orders_sent = orders_sent;
        prev_bot_fills_total = bot_fills_total;
        prev_bot_volume = bot_volume;
        prev_win_trades  = win_trades;
        prev_loss_trades = loss_trades;
        prev_win_ratio   = win_ratio;
        prev_profit_factor = profit_factor;
        prev_strategy_name = strategy_name;
        has_prev = true;
    }

    std::vector<float> heatmap_values;
    int heatmap_rows{50};
    int heatmap_cols{200};
    double heatmap_min_y{0.0};
    double heatmap_max_y{0.0};

    void push_heatmap_column(const std::vector<float>& col_data, double min_y, double max_y) {

        std::lock_guard lk(mu);
        if (heatmap_values.empty())
            heatmap_values.assign(heatmap_rows * heatmap_cols, 0.0f);
        for (int r = 0; r < heatmap_rows; ++r) {
            for (int c = 0; c < heatmap_cols - 1; ++c)
                heatmap_values[r * heatmap_cols + c] = heatmap_values[r * heatmap_cols + (c + 1)];
            heatmap_values[r * heatmap_cols + (heatmap_cols - 1)] = col_data[r];
        }
        heatmap_min_y = min_y;
        heatmap_max_y = max_y;
    }

    void _push_live_heatmap_column_locked() {
        if (live_bids.empty() && live_asks.empty()) return;

        double mid = 0.0;
        if (!live_bids.empty() && !live_asks.empty())
            mid = (live_bids.front().first + live_asks.front().first) * 0.5;
        else if (!live_bids.empty()) mid = live_bids.front().first;
        else                         mid = live_asks.front().first;

        double tick_size = std::max(0.01, std::round(mid * 0.0001 * 100.0) / 100.0);
        double half_span = (heatmap_rows / 2) * tick_size;
        double min_y = mid - half_span;
        double max_y = mid + half_span;

        std::vector<float> col(heatmap_rows, 0.0f);
        auto accumulate = [&](const std::vector<std::pair<double,double>>& levels) {
            for (auto& [px, qty] : levels) {
                int r = (int)std::round((px - min_y) / tick_size);
                if (r >= 0 && r < heatmap_rows)
                    col[r] += (float)qty;
            }
        };
        accumulate(live_bids);
        accumulate(live_asks);

        if (heatmap_values.empty())
            heatmap_values.assign(heatmap_rows * heatmap_cols, 0.0f);
        for (int r = 0; r < heatmap_rows; ++r) {
            for (int c = 0; c < heatmap_cols - 1; ++c)
                heatmap_values[r * heatmap_cols + c] = heatmap_values[r * heatmap_cols + (c + 1)];
            heatmap_values[r * heatmap_cols + (heatmap_cols - 1)] = col[r];
        }
        heatmap_min_y = min_y;
        heatmap_max_y = max_y;
    }
};

struct PositionTracker {
    double pos{0.0};
    double avg_price{0.0};
    uint64_t win_trades{0};
    uint64_t loss_trades{0};
    double gross_wins{0.0};
    double gross_losses{0.0};

    void update(Side side, double qty, double price) {
        if (pos == 0.0) {
            pos = (side == Side::Buy) ? qty : -qty;
            avg_price = price;
        } else if (pos > 0.0) {
            if (side == Side::Buy) {
                avg_price = (pos * avg_price + qty * price) / (pos + qty);
                pos += qty;
            } else {
                double closed_qty = std::min(qty, pos);
                double pnl = closed_qty * (price - avg_price);
                if (pnl > 0.0) {
                    win_trades++;
                    gross_wins += pnl;
                } else if (pnl < 0.0) {
                    loss_trades++;
                    gross_losses += std::abs(pnl);
                }
                pos -= qty;
                if (pos < 0.0) {
                    avg_price = price;
                }
            }
        } else {
            if (side == Side::Sell) {
                avg_price = (std::abs(pos) * avg_price + qty * price) / (std::abs(pos) + qty);
                pos -= qty;
            } else {
                double closed_qty = std::min(qty, std::abs(pos));
                double pnl = closed_qty * (avg_price - price);
                if (pnl > 0.0) {
                    win_trades++;
                    gross_wins += pnl;
                } else if (pnl < 0.0) {
                    loss_trades++;
                    gross_losses += std::abs(pnl);
                }
                pos += qty;
                if (pos > 0.0) {
                    avg_price = price;
                }
            }
        }
    }
};

class SimRunner {
public:
    explicit SimRunner(SimState& state) : state_(state) {}

    void start(const std::string& strategy, int ticks, double sigma,
               int seed, double initial_cash, bool enable_bots, std::function<void()> done_cb,
               bool csv_mode = false, int save_slot_idx = -1) {
        stop();
        stop_flag_.store(false);
        state_.done = false;
        state_.running = true;
        state_.tick_num = 0;
        state_.total_ticks = ticks;
        state_.strategy_name = strategy;
        state_.initial_cash = initial_cash;
        state_.equity.clear();
        state_.fill_log.clear();
        state_.bot_fill_log.clear();
        state_.heatmap_values.clear();
        state_.heatmap_min_y = 0.0;
        state_.heatmap_max_y = 0.0;
        state_.fills_total = state_.orders_sent = 0;
        state_.bot_fills_total = 0;
        state_.bot_volume = 0.0;
        state_.net_position = 0;
        state_.sharpe = state_.sortino = state_.calmar = state_.max_dd = 0;
        state_.correlation.clear();
        state_.sa_spy_pos = 0.0;
        state_.sa_qqq_pos = 0.0;
        pending_.clear();
        strat_pos_.clear();
        lat_rng_.seed((unsigned)seed ^ 0x9E3779B9u);
        slip_sum_ = qpos_sum_ = 0.0; slip_n_ = qpos_n_ = 0;
        cur_tick_ = 0; cur_mid_ = 0.0;
        tfees_ = treb_ = tborrow_ = 0.0; mk_fills_ = tk_fills_ = 0; cash_ref_ptr_ = nullptr;
        state_.avg_slippage = state_.avg_queue_pos = 0.0; state_.slip_samples = 0;
        state_.total_fees = state_.total_rebates = state_.total_borrow = 0.0;
        state_.maker_fills = state_.taker_fills = 0;
        state_.risk_halted = false; state_.var95 = 0.0;
        state_.gross_exposure = state_.net_exposure = 0.0;
        state_.realized_pnl = state_.unrealized_pnl = 0.0;
        state_.orders_blocked = 0;
        state_.corr_matrix.clear(); state_.asset_pos.clear(); state_.n_assets = 0;
        state_.delta_series.clear();
        state_.greeks_surface.clear(); state_.greeks_strikes.clear();
        state_.net_delta = state_.net_gamma = state_.net_vega = state_.net_theta = 0.0;
        state_.opt_position = state_.opt_hedge_pos = state_.opt_theo = 0.0;
        thread_ = std::thread([this, strategy, ticks, sigma, seed, initial_cash,
                               enable_bots, csv_mode, save_slot_idx, done_cb]{
            run_sim(strategy, ticks, sigma, seed, initial_cash, enable_bots, csv_mode);
            if (save_slot_idx >= 0 && save_slot_idx < 5) state_.save_to_compare(save_slot_idx);
            done_cb();
        });
    }

    void stop() {
        stop_flag_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    ~SimRunner() { stop(); }

private:
    void run_sim(const std::string& strategy, int ticks,
                  double sigma, int seed, double cash, bool enable_bots, bool csv_mode) {
        MatchingEngine eng;
        KillSwitch     ks;
        double         position = 0;
        double         pnl_cash = cash;
        int64_t        tick_n   = 0;
        cash_ref_ptr_ = &pnl_cash;
        {
            std::lock_guard lk(state_.mu);
            fees_on_   = state_.fees_enabled;
            taker_bps_ = state_.taker_fee_bps;
            maker_bps_ = state_.maker_rebate_bps;
            borrow_bps_= state_.borrow_bps_per_tick;
            risk_on_   = state_.risk_enabled;
            max_pos_   = state_.max_position;
            max_notional_ = state_.max_notional;
            max_loss_  = state_.max_loss;
        }
        init_cash_ = cash;
        halted_ = false; blocked_ = 0;

        std::string sym = "AAPL";
        bot_positions_.clear();
        bot_cash_ = 0.0;

        std::unordered_map<std::string, PositionTracker> trackers;
        {
            std::lock_guard lk(state_.mu);
            state_.win_trades = 0;
            state_.loss_trades = 0;
            state_.win_ratio = 0.0;
            state_.profit_factor = 1.0;
            state_.gross_wins = 0.0;
            state_.gross_losses = 0.0;
        }

        eng.on_fill = [&](const Fill& f) {
            if (f.maker_is_bot) {
                double maker_sign = (f.aggressor == Side::Buy) ? -1.0 : 1.0;
                bot_positions_[f.symbol] += maker_sign * f.qty;
                bot_cash_                -= maker_sign * f.qty * f.price;
            }
            if (f.taker_is_bot) {
                double taker_sign = (f.aggressor == Side::Buy) ? 1.0 : -1.0;
                bot_positions_[f.symbol] += taker_sign * f.qty;
                bot_cash_                -= taker_sign * f.qty * f.price;
            }

             if (!f.maker_is_bot || !f.taker_is_bot) {

                Side strat_side = (f.taker_is_bot == false) ? f.aggressor : ((f.aggressor == Side::Buy) ? Side::Sell : Side::Buy);
                double side_sign = (strat_side == Side::Buy) ? 1.0 : -1.0;
                position += side_sign * f.qty;
                pnl_cash  -= side_sign * f.qty * f.price;
                strat_pos_[f.symbol.empty() ? sym : f.symbol] += side_sign * f.qty;

                if (fees_on_) {
                    double notional = (double)f.qty * f.price;
                    if (!f.taker_is_bot) { double fee = notional*taker_bps_*1e-4; pnl_cash -= fee; tfees_ += fee; }
                    if (!f.maker_is_bot) { double reb = notional*maker_bps_*1e-4; pnl_cash += reb; treb_ += reb; }
                }
                if (!f.taker_is_bot) ++tk_fills_; else if (!f.maker_is_bot) ++mk_fills_;

                auto& tracker = trackers[f.symbol.empty() ? sym : f.symbol];
                tracker.update(strat_side, f.qty, f.price);

                {
                    std::lock_guard lk(state_.mu);
                    uint64_t w = 0, l = 0;
                    double gw = 0, gl = 0;
                    for (const auto& [s, t] : trackers) {
                        w += t.win_trades;
                        l += t.loss_trades;
                        gw += t.gross_wins;
                        gl += t.gross_losses;
                    }
                    state_.win_trades = w;
                    state_.loss_trades = l;
                    state_.gross_wins = gw;
                    state_.gross_losses = gl;
                    state_.total_fees = tfees_; state_.total_rebates = treb_;
                    state_.taker_fills = tk_fills_; state_.maker_fills = mk_fills_;
                }

                SimState::FillRow row;
                row.id    = f.taker_id;
                row.sym   = f.symbol.empty() ? sym : f.symbol;
                row.side  = (strat_side == Side::Buy) ? "BUY" : "SELL";
                row.qty   = f.qty;
                row.price = f.price;
                row.ts_ns = f.ts_ns;
                state_.push_fill(std::move(row));
            } else {

                std::lock_guard lk(state_.mu);
                state_.bot_fills_total++;
                state_.bot_volume += f.qty * f.price;
                SimState::BotFillRow row;
                row.id    = f.taker_id;
                row.sym   = f.symbol.empty() ? sym : f.symbol;
                row.side  = (f.aggressor == Side::Buy) ? "BUY" : "SELL";
                row.qty   = f.qty;
                row.price = f.price;
                row.ts_ns = f.ts_ns;
                if (state_.bot_fill_log.size() >= 200) state_.bot_fill_log.pop_front();
                state_.bot_fill_log.push_back(std::move(row));
            }
        };

        eng.on_book_update = [&](const BookSnapshot& snap) {
            auto* lob = eng.book(snap.symbol);
            if (!lob) return;
            state_.set_book(snap.best_bid, snap.best_ask,
                             snap.bid_sz, snap.ask_sz,
                             lob->bid_depth(8), lob->ask_depth(8));
        };

        if (strategy == "mm")
            run_mm(eng, ks, sym, ticks, sigma, seed, cash, position, pnl_cash, tick_n, enable_bots, csv_mode);
        else if (strategy == "stat_arb")
            run_sa(eng, ks, ticks, sigma, seed, cash, position, pnl_cash, tick_n, enable_bots, csv_mode);
        else if (strategy == "twap")
            run_twap(eng, ks, sym, ticks, sigma, seed, cash, position, pnl_cash, tick_n, enable_bots, csv_mode);
        else if (strategy == "momentum")
            run_generic_custom(eng, sym, ticks, sigma, seed, cash, position, pnl_cash,
                               enable_bots, csv_mode, "Momentum", state_.param_snapshot("momentum"),
                               [](StrategyContext& ctx, double&, double&, std::deque<double>& prices, const ParamMap& P) -> StrategyOrder {
                                   return run_momentum_tick(ctx, prices, P);
                               });
        else if (strategy == "mean_rev")
            run_generic_custom(eng, sym, ticks, sigma, seed, cash, position, pnl_cash,
                               enable_bots, csv_mode, "Mean Reversion (BB)", state_.param_snapshot("mean_rev"),
                               [](StrategyContext& ctx, double&, double&, std::deque<double>& prices, const ParamMap& P) -> StrategyOrder {
                                   return run_mean_rev_tick(ctx, prices, P);
                               });
        else if (strategy == "rsi")
            run_generic_custom(eng, sym, ticks, sigma, seed, cash, position, pnl_cash,
                               enable_bots, csv_mode, "RSI Reversal", state_.param_snapshot("rsi"),
                               [](StrategyContext& ctx, double&, double&, std::deque<double>& prices, const ParamMap& P) -> StrategyOrder {
                                   return run_rsi_tick(ctx, prices, P);
                               });
        else if (strategy == "breakout")
            run_generic_custom(eng, sym, ticks, sigma, seed, cash, position, pnl_cash,
                               enable_bots, csv_mode, "Breakout (Donchian)", state_.param_snapshot("breakout"),
                               [](StrategyContext& ctx, double& entry, double& dirf, std::deque<double>& prices, const ParamMap& P) -> StrategyOrder {
                                   return run_breakout_tick(ctx, prices, entry, dirf, P);
                               });
        else if (strategy == "portfolio")
            run_portfolio(eng, ticks, sigma, seed, cash, position, pnl_cash, enable_bots, csv_mode);
        else if (strategy == "options")
            run_options(eng, ticks, sigma, seed, cash, position, pnl_cash, enable_bots, csv_mode);
        else if (strategy == "custom_cpp") {
            run_custom_cpp(eng, sym, ticks, sigma, seed, cash, position, pnl_cash,
                           enable_bots, csv_mode);
        }

        state_.compute_metrics();
        state_.running = false;
        state_.done    = true;
    }

    uint64_t do_submit(MatchingEngine& eng, Order o) {
        const uint64_t id = o.id;

        if (risk_on_) {
            double mtm = cash_ref_ptr_ ? *cash_ref_ptr_ : init_cash_;
            for (auto& [s, p] : strat_pos_) mtm += p * cur_mid_;
            if (max_loss_ > 0 && (mtm - init_cash_) < -max_loss_) halted_ = true;
            double proj = strat_pos_[o.symbol] + (o.side==Side::Buy ? (double)o.qty : -(double)o.qty);
            bool breach = halted_
                || (max_pos_ > 0 && std::abs(proj) > max_pos_)
                || (max_notional_ > 0 && std::abs(proj) * cur_mid_ > max_notional_);
            if (breach) {
                ++blocked_;
                std::lock_guard lk(state_.mu);
                state_.orders_blocked = blocked_;
                state_.risk_halted = halted_;
                return id;
            }
        }
        ++state_.orders_sent;
        bool lat; double lo, hi;
        {
            std::lock_guard lk(state_.mu);
            lat = state_.latency_enabled; lo = state_.latency_min_ms; hi = state_.latency_max_ms;
        }
        if (!lat) { eng.submit(std::move(o)); return id; }

        if (auto* lob = eng.book(o.symbol)) {
            qpos_sum_ += (double)lob->qty_at(o.price);
            ++qpos_n_;
        }

        double mean_ms = std::max(1e-6, (lo + hi) * 0.5);
        std::lognormal_distribution<double> d(std::log(mean_ms), 0.5);
        double ms = std::clamp(d(lat_rng_), lo, hi);
        pending_.push_back({ std::move(o), cur_tick_ + (int)std::lround(ms), cur_mid_ });
        return id;
    }

    void do_cancel(MatchingEngine& eng, uint64_t id) {
        if (!id) return;
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if (it->o.id == id) { pending_.erase(it); return; }
        eng.cancel(id);
    }

    void flush_pending(MatchingEngine& eng) {

        if (fees_on_ && borrow_bps_ > 0 && cash_ref_ptr_ && cur_mid_ > 0) {
            double short_notional = 0;
            for (auto& [s, p] : strat_pos_) if (p < 0) short_notional += -p * cur_mid_;
            double cost = short_notional * borrow_bps_ * 1e-4;
            *cash_ref_ptr_ -= cost; tborrow_ += cost;
        }
        if (cur_mid_ > 0 && !strat_pos_.empty()) {
            double gross = 0, net = 0;
            for (auto& [s, p] : strat_pos_) { gross += std::abs(p) * cur_mid_; net += p * cur_mid_; }
            std::lock_guard lk(state_.mu);
            state_.gross_exposure = gross; state_.net_exposure = net;
        }
        if (pending_.empty()) {
            std::lock_guard lk(state_.mu);
            if (qpos_n_) state_.avg_queue_pos = qpos_sum_/qpos_n_;
            if (tborrow_) state_.total_borrow = tborrow_;
            return;
        }
        std::deque<Pending> keep;
        while (!pending_.empty()) {
            Pending p = std::move(pending_.front()); pending_.pop_front();
            if (p.release_tick <= cur_tick_) {
                double ref = p.ref_mid;
                auto res = eng.submit(std::move(p.o));
                double notional = 0, q = 0;
                for (auto& f : res.fills) { notional += f.price * f.qty; q += f.qty; }
                if (q > 0 && ref > 0) { slip_sum_ += std::abs(notional/q - ref); ++slip_n_; }
            } else {
                keep.push_back(std::move(p));
            }
        }
        pending_ = std::move(keep);
        std::lock_guard lk(state_.mu);
        if (slip_n_) { state_.avg_slippage = slip_sum_/slip_n_; state_.slip_samples = slip_n_; }
        if (qpos_n_)   state_.avg_queue_pos = qpos_sum_/qpos_n_;
        if (tborrow_)  state_.total_borrow = tborrow_;
    }

    void run_mm(MatchingEngine& eng, KillSwitch& ks, const std::string& sym,
                int ticks, double sigma, int seed,
                double cash, double& pos, double& cash_ref, int64_t& tick_n, bool enable_bots, bool csv_mode) {
        SyntheticFeed feed({sym, 150.0, 0.0, sigma, 0.01, 2.0, (unsigned)seed});
        CsvTickReplay csv_feed;
        if (csv_mode) {
            csv_feed.load(state_.csv_path);
        }
        BotSimulator bots(eng, sym, 0.01);
        ParamMap P = state_.param_snapshot("mm");
        const double gamma_tweaked = P["gamma"];
        const double kappa_tweaked = P["kappa"];
        double inv=0;
        uint64_t bid_id=0, ask_id=0;
        double last_mid=0;
        const int order_qty=100;
        const int MAX_INV=1000;

        auto submit = [&](Side side, double price, uint32_t qty) -> uint64_t {
            Order o; o.id=eng.next_order_id(); o.symbol=sym; o.side=side;
            o.type=OrdType::PostOnly;
            o.price=price; o.qty=qty;
            return do_submit(eng, std::move(o));
        };
        auto cancel_id = [&](uint64_t id){ do_cancel(eng, id); };

        int total_ticks = csv_mode ? csv_feed.tick_count() : ticks;
        state_.total_ticks = total_ticks;

        for (int i=0; i<total_ticks && !stop_flag_.load(); ++i) {
            TickEvent tick;
            if (csv_mode) {
                tick = csv_feed.ticks()[i];
            } else {
                tick = feed.next_tick();
            }
            double mid = tick.mid();
            cur_tick_ = i; cur_mid_ = mid;
            flush_pending(eng);
            if (enable_bots) {
                double spd_t = csv_mode ? std::max(1.0, (tick.ask - tick.bid) / 0.01) : 2.0;
                bots.step(mid, spd_t);
            }

            inv = pos;

            if (std::abs(inv) >= MAX_INV) {
                cancel_id(bid_id); cancel_id(ask_id);
                bid_id=ask_id=0;
            } else {
                double T   = std::max(0.01, 1.0-(double)i/total_ticks);
                double r   = mid - inv*gamma_tweaked*sigma*sigma*T;
                double spd = std::max(0.005, gamma_tweaked*sigma*sigma*T/2.0 +
                                             std::log(1.0+gamma_tweaked/kappa_tweaked)/gamma_tweaked);

                bool reprice = (last_mid==0 || std::abs(mid-last_mid) > 0.005);
                if (reprice) {
                    cancel_id(bid_id); cancel_id(ask_id);
                    bid_id = submit(Side::Buy,  std::round((r-spd)*100)/100, order_qty);
                    ask_id = submit(Side::Sell, std::round((r+spd)*100)/100, order_qty);
                    last_mid = mid;
                }
            }

            double mark = mid;
            double eq   = cash_ref + pos*mark;
            double bot_eq = cash + bot_cash_ + bot_positions_[sym] * mark;
            capture_heatmap_frame(eng, sym, mid, 0.01);
            state_.push_equity(eq, bot_eq);
            state_.tick_num = i+1;
            if ((i%50)==0) state_.compute_metrics();
            if (state_.delay_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(state_.delay_us));
            }
        }
    }

    void run_sa(MatchingEngine& eng, KillSwitch& ks,
                int ticks, double sigma, int seed,
                double cash, double& pos, double& cash_ref, int64_t& tick_n, bool enable_bots, bool csv_mode) {
        CsvTickReplay csv_feed;
        if (csv_mode) {
            csv_feed.load(state_.csv_path);
        }
        OUSpreadFeed feed("SPY","QQQ", 450.0, 0.08, 0.0, 0.05, sigma, 0.01, 2.0, 1.0, (unsigned)seed);
        BotSimulator bots_a(eng, "SPY", 0.01);
        BotSimulator bots_b(eng, "QQQ", 0.01);

        const int W=60; std::deque<double> buf;
        std::string mode="flat";
        const int qty=100;
        double pos_a=0,pos_b=0;
        std::unordered_map<std::string,double> mids;

        auto submit=[&](const std::string& sym,Side side,double price,uint32_t q){
            Order o; o.id=eng.next_order_id(); o.symbol=sym; o.side=side;
            o.type=OrdType::Market; o.price=price; o.qty=q;
            do_submit(eng, std::move(o));
        };

        int total_ticks = csv_mode ? csv_feed.tick_count() : ticks;
        state_.total_ticks = total_ticks;

        for (int i=0; i<total_ticks && !stop_flag_.load(); ++i) {
            double mid_spy = 0.0, mid_qqq = 0.0;
            double bid_spy = 0.0, ask_spy = 0.0;
            double bid_qqq = 0.0, ask_qqq = 0.0;
            if (csv_mode) {
                const auto& tick = csv_feed.ticks()[i];
                mids[tick.symbol] = tick.mid();
                if (tick.symbol == "SPY") {
                    bid_spy = tick.bid; ask_spy = tick.ask;
                } else if (tick.symbol == "QQQ") {
                    bid_qqq = tick.bid; ask_qqq = tick.ask;
                }
                if (mids.find("SPY") == mids.end() || mids.find("QQQ") == mids.end()) {
                    continue;
                }
                mid_spy = mids["SPY"];
                mid_qqq = mids["QQQ"];
                if (enable_bots) {
                    if (tick.symbol == "SPY") {
                        double spd_t = std::max(1.0, (ask_spy - bid_spy) / 0.01);
                        bots_a.step(mid_spy, spd_t);
                    } else if (tick.symbol == "QQQ") {
                        double spd_t = std::max(1.0, (ask_qqq - bid_qqq) / 0.01);
                        bots_b.step(mid_qqq, spd_t);
                    }
                }
            } else {
                auto [ta,tb] = feed.next();
                mids["SPY"]=ta.mid(); mids["QQQ"]=tb.mid();
                mid_spy = mids["SPY"];
                mid_qqq = mids["QQQ"];
                if (enable_bots) {
                    bots_a.step(mid_spy, 2.0);
                    bots_b.step(mid_qqq, 2.0);
                }
            }

            cur_tick_ = i; cur_mid_ = mid_spy; flush_pending(eng);
            double sp = mid_spy - mid_qqq;
            buf.push_back(sp);
            if ((int)buf.size()>W) buf.pop_front();
            double z=0;
            if ((int)buf.size()==W) {
                double m=0; for(auto v:buf) m+=v; m/=W;
                double s=0; for(auto v:buf) s+=(v-m)*(v-m); s=std::sqrt(s/W);
                z = s>0?(sp-m)/s:0;
            }

            static std::deque<double> spy_buf, qqq_buf;
            if (i == 0) { spy_buf.clear(); qqq_buf.clear(); }
            spy_buf.push_back(mid_spy);
            qqq_buf.push_back(mid_qqq);
            if ((int)spy_buf.size() > W) {
                spy_buf.pop_front();
                qqq_buf.pop_front();
            }

            double corr = 0.0;
            if ((int)spy_buf.size() == W) {
                double sum_x = 0.0, sum_y = 0.0;
                for (int j = 0; j < W; ++j) {
                    sum_x += spy_buf[j];
                    sum_y += qqq_buf[j];
                }
                double mean_x = sum_x / W;
                double mean_y = sum_y / W;

                double num = 0.0, den_x = 0.0, den_y = 0.0;
                for (int j = 0; j < W; ++j) {
                    double dx = spy_buf[j] - mean_x;
                    double dy = qqq_buf[j] - mean_y;
                    num += dx * dy;
                    den_x += dx * dx;
                    den_y += dy * dy;
                }
                double den = std::sqrt(den_x * den_y);
                corr = den > 1e-9 ? num / den : 0.0;
            }

            if (mode=="flat" && (int)buf.size()==W) {
                if (z>2.0) {
                    submit("SPY",Side::Sell,mid_spy,qty);
                    submit("QQQ",Side::Buy, mid_qqq,qty);
                    pos_a-=qty; pos_b+=qty; mode="short";
                } else if (z<-2.0) {
                    submit("SPY",Side::Buy, mid_spy,qty);
                    submit("QQQ",Side::Sell,mid_qqq,qty);
                    pos_a+=qty; pos_b-=qty; mode="long";
                }
            } else if (mode=="short" && std::abs(z)<0.3) {
                submit("SPY",Side::Buy, mid_spy,qty);
                submit("QQQ",Side::Sell,mid_qqq,qty);
                pos_a+=qty; pos_b-=qty; mode="flat";
            } else if (mode=="long" && std::abs(z)<0.3) {
                submit("SPY",Side::Sell,mid_spy,qty);
                submit("QQQ",Side::Buy, mid_qqq,qty);
                pos_a-=qty; pos_b+=qty; mode="flat";
            }

            {
                std::lock_guard lk(state_.mu);
                state_.sa_spy_pos = pos_a;
                state_.sa_qqq_pos = pos_b;
                state_.correlation.push_back(corr);
            }

            double eq = cash_ref + pos_a*mid_spy + pos_b*mid_qqq;
            double bot_eq = cash + bot_cash_ + bot_positions_["SPY"] * mid_spy + bot_positions_["QQQ"] * mid_qqq;
            capture_heatmap_frame(eng, "SPY", mid_spy, 0.01);
            state_.push_equity(eq, bot_eq);
            state_.tick_num = i+1;
            if ((i%50)==0) state_.compute_metrics();
            if (state_.delay_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(state_.delay_us));
            }
        }
    }

    void run_twap(MatchingEngine& eng, KillSwitch& ks, const std::string& sym,
                  int ticks, double sigma, int seed,
                  double cash, double& pos, double& cash_ref, int64_t& tick_n, bool enable_bots, bool csv_mode) {
        SyntheticFeed feed({sym, 200.0, 0.0, sigma, 0.01, 2.0, (unsigned)seed});
        CsvTickReplay csv_feed;
        if (csv_mode) {
            csv_feed.load(state_.csv_path);
        }
        BotSimulator bots(eng, sym, 0.01);
        const int total_qty=5000, n_slices=20;
        int total_ticks = csv_mode ? csv_feed.tick_count() : ticks;
        state_.total_ticks = total_ticks;
        int slice_ticks = total_ticks/n_slices;
        if (slice_ticks <= 0) slice_ticks = 1;
        int slices_sent=0, filled=0;

        auto submit=[&](Side side,double price,uint32_t qty){
            Order o; o.id=eng.next_order_id(); o.symbol=sym; o.side=side;
            o.type=OrdType::Limit; o.price=price; o.qty=qty;
            do_submit(eng, std::move(o));
        };

        for(int i=0; i<total_ticks && !stop_flag_.load(); ++i) {
            TickEvent tick;
            if (csv_mode) {
                tick = csv_feed.ticks()[i];
            } else {
                tick = feed.next_tick();
            }
            if (enable_bots) {
                double spd_t = csv_mode ? std::max(1.0, (tick.ask - tick.bid) / 0.01) : 2.0;
                bots.step(tick.mid(), spd_t);
            }
            cur_tick_ = i; cur_mid_ = tick.mid(); flush_pending(eng);
            filled = pos;
            if(i%slice_ticks==0 && slices_sent<n_slices && filled<total_qty) {
                int remaining = total_qty-filled;
                int qty = (slices_sent==n_slices-1) ? remaining :
                           std::min(total_qty/n_slices, remaining);
                submit(Side::Buy, tick.ask, qty);
                ++slices_sent;
            }
            double eq = cash_ref + pos*tick.mid();
            double bot_eq = cash + bot_cash_ + bot_positions_[sym] * tick.mid();
            capture_heatmap_frame(eng, sym, tick.mid(), 0.01);
            state_.push_equity(eq, bot_eq);
            state_.tick_num=i+1;
            if((i%50)==0) state_.compute_metrics();
            if (state_.delay_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(state_.delay_us));
            }
        }
    }

    void capture_heatmap_frame(MatchingEngine& eng, const std::string& sym, double mid, double tick_size) {
        auto* lob = eng.book(sym);
        std::vector<float> col_data(state_.heatmap_rows, 0.0f);
        double half_span = (state_.heatmap_rows / 2) * tick_size;
        double min_y = mid - half_span;
        double max_y = mid + half_span;

        if (lob) {
            for (int r = 0; r < state_.heatmap_rows; ++r) {
                double price = mid + (r - state_.heatmap_rows / 2) * tick_size;
                price = std::round(price / tick_size) * tick_size;
                col_data[r] = static_cast<float>(lob->qty_at(price));
            }
        }
        state_.push_heatmap_column(col_data, min_y, max_y);
    }

    static StrategyOrder run_momentum_tick(StrategyContext& ctx, std::deque<double>& prices,
                                           const ParamMap& P) {
        const int    LOOKBACK = (int)param_or_default("momentum", P, "lookback");
        const double THRESH   = param_or_default("momentum", P, "thresh_bps") * 1e-4;
        const double SZ       = param_or_default("momentum", P, "size");
        const double MAX_P    = param_or_default("momentum", P, "max_pos");
        prices.push_back(ctx.mid);
        if ((int)prices.size() > LOOKBACK + 1) prices.pop_front();
        if ((int)prices.size() < LOOKBACK) return { Action::None, 0, 0 };
        double ret = (ctx.mid - prices.front()) / prices.front();
        if (ret > THRESH  && ctx.position < MAX_P)  return { Action::Buy,  SZ, ctx.ask };
        if (ret < -THRESH && ctx.position > -MAX_P) return { Action::Sell, SZ, ctx.bid };
        return { Action::None, 0, 0 };
    }

    static StrategyOrder run_mean_rev_tick(StrategyContext& ctx, std::deque<double>& prices,
                                           const ParamMap& P) {
        const int    W     = (int)param_or_default("mean_rev", P, "window");
        const double MULT  = param_or_default("mean_rev", P, "mult");
        const double SZ    = param_or_default("mean_rev", P, "size");
        const double MAX_P = param_or_default("mean_rev", P, "max_pos");
        prices.push_back(ctx.mid);
        if ((int)prices.size() > W) prices.pop_front();
        if ((int)prices.size() < W/2) return { Action::None, 0, 0 };
        double mean = 0; for (double p : prices) mean += p; mean /= prices.size();
        double var  = 0; for (double p : prices) var += (p-mean)*(p-mean); var /= prices.size();
        double sd   = std::sqrt(var);
        if (ctx.mid > mean + MULT*sd && ctx.position > -MAX_P) return { Action::Sell, SZ, ctx.bid };
        if (ctx.mid < mean - MULT*sd && ctx.position <  MAX_P) return { Action::Buy,  SZ, ctx.ask };
        if (std::abs(ctx.mid - mean) < sd*0.3 && ctx.position != 0)
            return { Action::Close, std::abs(ctx.position), ctx.mid };
        return { Action::None, 0, 0 };
    }

    static StrategyOrder run_rsi_tick(StrategyContext& ctx, std::deque<double>& prices,
                                      const ParamMap& P) {
        const int    Per   = (int)param_or_default("rsi", P, "period");
        const double OS    = param_or_default("rsi", P, "oversold");
        const double OB    = param_or_default("rsi", P, "overbought");
        const double SZ    = param_or_default("rsi", P, "size");
        const double MAX_P = 120.0;
        prices.push_back(ctx.mid);
        if ((int)prices.size() > Per + 5) prices.pop_front();
        if ((int)prices.size() < Per) return { Action::None, 0, 0 };
        double gains = 0, losses = 0;
        for (size_t i = 1; i < prices.size(); ++i) {
            double d = prices[i] - prices[i-1];
            if (d > 0) gains += d; else losses -= d;
        }
        int n = (int)prices.size() - 1;
        double avg_g = gains/n, avg_l = losses/n;
        double rsi = (avg_l == 0.0) ? 100.0 : 100.0 - 100.0/(1.0 + avg_g/avg_l);
        if (rsi < OS && ctx.position < MAX_P)  return { Action::Buy,  SZ, ctx.ask };
        if (rsi > OB && ctx.position > -MAX_P) return { Action::Sell, SZ, ctx.bid };
        return { Action::None, 0, 0 };
    }

    static StrategyOrder run_breakout_tick(StrategyContext& ctx, std::deque<double>& prices,
                                           double& entry, double& dirf, const ParamMap& P) {
        const int    CP    = (int)param_or_default("breakout", P, "channel");
        const double SL    = param_or_default("breakout", P, "stop_pct") * 0.01;
        const double SZ    = param_or_default("breakout", P, "size");
        const double MAX_P = param_or_default("breakout", P, "max_pos");
        int dir = (int)dirf;
        prices.push_back(ctx.mid);
        if ((int)prices.size() > CP) prices.pop_front();
        if ((int)prices.size() < CP) return { Action::None, 0, 0 };
        double hi = *std::max_element(prices.begin(), prices.end());
        double lo = *std::min_element(prices.begin(), prices.end());

        if (dir == 1 && entry > 0 && ctx.mid < entry*(1.0-SL)) {
            dirf = 0; entry = 0;
            return { Action::Close, std::abs(ctx.position), ctx.bid };
        }
        if (dir == -1 && entry > 0 && ctx.mid > entry*(1.0+SL)) {
            dirf = 0; entry = 0;
            return { Action::Close, std::abs(ctx.position), ctx.ask };
        }
        if (ctx.ask >= hi && ctx.position < MAX_P && dir != 1) {
            dirf = 1; entry = ctx.ask; return { Action::Buy, SZ, ctx.ask };
        }
        if (ctx.bid <= lo && ctx.position > -MAX_P && dir != -1) {
            dirf = -1; entry = ctx.bid; return { Action::Sell, SZ, ctx.bid };
        }
        return { Action::None, 0, 0 };
    }

    static OrdType map_ord_kind(int ot) {
        switch (ot) {
            case 0: return OrdType::Market;   case 2: return OrdType::IOC;
            case 3: return OrdType::FOK;      case 4: return OrdType::PostOnly;
            case 5: return OrdType::Stop;     case 6: return OrdType::StopLimit;
            case 7: return OrdType::Iceberg;  default: return OrdType::Limit;
        }
    }

    void run_generic_custom(MatchingEngine& eng, const std::string& sym,
                             int ticks, double sigma, int seed,
                             double cash, double& pos, double& cash_ref,
                             bool enable_bots, bool csv_mode,
                             const char*, ParamMap P,
                             std::function<StrategyOrder(StrategyContext&, double&, double&, std::deque<double>&, const ParamMap&)> on_tick_fn) {
        SyntheticFeed feed({sym, 150.0, 0.0, sigma, 0.01, 2.0, (unsigned)seed});
        CsvTickReplay csv_feed;
        if (csv_mode) csv_feed.load(state_.csv_path);
        BotSimulator  bots(eng, sym, 0.01);
        int total_ticks = csv_mode ? (int)csv_feed.tick_count() : ticks;
        state_.total_ticks = total_ticks;
        std::deque<double> price_buf;
        double s1 = 0, s2 = 0;

        auto submit = [&](Side side, double price, uint32_t qty) {
            Order o; o.id = eng.next_order_id(); o.symbol = sym; o.side = side;
            o.type = OrdType::Limit; o.price = price; o.qty = qty;
            do_submit(eng, std::move(o));
        };

        for (int i = 0; i < total_ticks && !stop_flag_.load(); ++i) {
            TickEvent tick = csv_mode ? csv_feed.ticks()[i] : feed.next_tick();
            cur_tick_ = i; cur_mid_ = tick.mid(); flush_pending(eng);
            if (enable_bots) bots.step(tick.mid(), 2.0);

            StrategyContext ctx;
            ctx.bid = tick.bid; ctx.ask = tick.ask; ctx.mid = tick.mid();
            ctx.spread = tick.ask - tick.bid;
            ctx.volatility = sigma;
            ctx.tick_num = (uint64_t)i;
            ctx.position = pos;
            ctx.cash = cash_ref;
            ctx.unrealised_pnl = pos * tick.mid();

            StrategyOrder order = on_tick_fn(ctx, s1, s2, price_buf, P);

            if (order.action == Action::Buy && order.qty > 0) {
                double px = order.price > 0 ? order.price : tick.ask;
                submit(Side::Buy, px, (uint32_t)std::round(order.qty));
            } else if (order.action == Action::Sell && order.qty > 0) {
                double px = order.price > 0 ? order.price : tick.bid;
                submit(Side::Sell, px, (uint32_t)std::round(order.qty));
            } else if (order.action == Action::Close && pos != 0) {
                double px = pos > 0 ? tick.bid : tick.ask;
                Side side  = pos > 0 ? Side::Sell : Side::Buy;
                submit(side, px, (uint32_t)std::round(std::abs(pos)));
            }

            double eq     = cash_ref + pos * tick.mid();
            double bot_eq = cash + bot_cash_ + bot_positions_[sym] * tick.mid();
            capture_heatmap_frame(eng, sym, tick.mid(), 0.01);
            state_.push_equity(eq, bot_eq);
            state_.tick_num = i + 1;
            if ((i % 50) == 0) state_.compute_metrics();
            if (state_.delay_us > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(state_.delay_us));
        }
    }

    void run_portfolio(MatchingEngine& eng, int ticks, double sigma, int seed,
                       double cash, double& pos, double& cash_ref,
                       bool enable_bots, bool csv_mode);

    void run_options(MatchingEngine& eng, int ticks, double sigma, int seed,
                     double cash, double& pos, double& cash_ref,
                     bool enable_bots, bool csv_mode);

    void run_custom_cpp(MatchingEngine& eng, const std::string& sym,
                        int ticks, double sigma, int seed,
                        double cash, double& pos, double& cash_ref,
                        bool enable_bots, bool csv_mode) {
        std::string dylib = state_.custom_dylib_path;
        if (dylib.empty()) return;

        void* handle = dlopen(dylib.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {

            std::lock_guard lk(state_.mu);
            state_.strategy_name = std::string("[LOAD ERROR] ") + (dlerror() ? dlerror() : "?");
            return;
        }

        using OnTickFn  = StrategyOrder(*)(StrategyContext);
        using NameFn    = const char*(*)();
        using OnInitFn  = void(*)(int, double);

        auto on_tick_fn  = (OnTickFn) dlsym(handle, "on_tick");
        auto name_fn     = (NameFn)   dlsym(handle, "strategy_name");
        auto on_init_fn  = (OnInitFn) dlsym(handle, "on_init");

        if (!on_tick_fn) { dlclose(handle); return; }
        if (name_fn) {
            std::lock_guard lk(state_.mu);
            state_.strategy_name = name_fn();
        }
        if (on_init_fn) on_init_fn(ticks, cash);

        SyntheticFeed feed({sym, 150.0, 0.0, sigma, 0.01, 2.0, (unsigned)seed});
        CsvTickReplay csv_feed;
        if (csv_mode) csv_feed.load(state_.csv_path);
        BotSimulator  bots(eng, sym, 0.01);
        int total_ticks = csv_mode ? (int)csv_feed.tick_count() : ticks;
        state_.total_ticks = total_ticks;

        auto submit = [&](Side side, double price, uint32_t qty) {
            Order o; o.id = eng.next_order_id(); o.symbol = sym; o.side = side;
            o.type = OrdType::Limit; o.price = price; o.qty = qty;
            do_submit(eng, std::move(o));
        };

        for (int i = 0; i < total_ticks && !stop_flag_.load(); ++i) {
            TickEvent tick = csv_mode ? csv_feed.ticks()[i] : feed.next_tick();
            cur_tick_ = i; cur_mid_ = tick.mid(); flush_pending(eng);
            if (enable_bots) bots.step(tick.mid(), 2.0);

            StrategyContext ctx;
            ctx.bid = tick.bid; ctx.ask = tick.ask; ctx.mid = tick.mid();
            ctx.spread = tick.ask - tick.bid;
            ctx.volatility = sigma;
            ctx.tick_num = (uint64_t)i;
            ctx.position = pos;
            ctx.cash = cash_ref;
            ctx.unrealised_pnl = pos * tick.mid();

            StrategyOrder order = on_tick_fn(ctx);

            auto submit_typed = [&](Side side, double price, uint32_t qty, const StrategyOrder& so){
                Order o; o.id = eng.next_order_id(); o.symbol = sym; o.side = side;
                o.type = map_ord_kind(so.ord_type); o.price = price; o.qty = qty;
                o.stop_price = so.stop_price; o.display_qty = (uint32_t)std::round(so.display_qty);
                do_submit(eng, std::move(o));
            };

            if (order.action == Action::Buy && order.qty > 0) {
                double px = order.price > 0 ? order.price : tick.ask;
                submit_typed(Side::Buy, px, (uint32_t)std::round(order.qty), order);
            } else if (order.action == Action::Sell && order.qty > 0) {
                double px = order.price > 0 ? order.price : tick.bid;
                submit_typed(Side::Sell, px, (uint32_t)std::round(order.qty), order);
            } else if (order.action == Action::Close && pos != 0) {
                double px = pos > 0 ? tick.bid : tick.ask;
                Side side  = pos > 0 ? Side::Sell : Side::Buy;
                submit(side, px, (uint32_t)std::round(std::abs(pos)));
            }

            double eq     = cash_ref + pos * tick.mid();
            double bot_eq = cash + bot_cash_ + bot_positions_[sym] * tick.mid();
            capture_heatmap_frame(eng, sym, tick.mid(), 0.01);
            state_.push_equity(eq, bot_eq);
            state_.tick_num = i + 1;
            if ((i % 50) == 0) state_.compute_metrics();
            if (state_.delay_us > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(state_.delay_us));
        }
        dlclose(handle);
    }

    SimState&            state_;
    std::atomic<bool>    stop_flag_{false};
    std::thread          thread_;
    std::unordered_map<std::string, double> bot_positions_;
    std::unordered_map<std::string, double> strat_pos_;
    double               bot_cash_{0.0};

    struct Pending { Order o; int release_tick; double ref_mid; };
    std::deque<Pending> pending_;
    std::mt19937        lat_rng_{0x515};
    int    cur_tick_{0};
    double cur_mid_{0.0};
    double slip_sum_{0.0}, qpos_sum_{0.0};
    uint64_t slip_n_{0}, qpos_n_{0};

    bool   fees_on_{false};
    double taker_bps_{0}, maker_bps_{0}, borrow_bps_{0};
    double tfees_{0}, treb_{0}, tborrow_{0};
    uint64_t mk_fills_{0}, tk_fills_{0};
    double* cash_ref_ptr_{nullptr};

    bool   risk_on_{false};
    bool   halted_{false};
    double max_pos_{0}, max_notional_{0}, max_loss_{0};
    double init_cash_{0};
    uint64_t blocked_{0};
};

}

#include "hft_simulator/sim_core_portfolio.hpp"
#include "hft_simulator/sim_core_options.hpp"
