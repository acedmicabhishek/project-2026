#include "hft_simulator/ui.h"
#include "hft_simulator/order.hpp"
#include "hft_simulator/orderbook.hpp"
#include "hft_simulator/matching.hpp"
#include "hft_simulator/feed.hpp"
#include "hft_simulator/risk.hpp"
#include "hft_simulator/metrics.hpp"
#include "hft_simulator/editor.hpp"

#include <gtkmm.h>
#include <cairomm/cairomm.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <memory>
#include <functional>
#include <csignal>
#include <cstdio>
#include <utility>

static void log_msg(const std::string& m) {
    std::ofstream f("hft_gui.log", std::ios::app);
    if (f) f << m << "\n";
}

namespace hft {

// ── Shared simulation state (written by sim thread, read by UI) ──────────────

struct SimState {
    std::mutex mu;

    // Book
    double best_bid{0}, best_ask{0};
    uint32_t bid_sz{0}, ask_sz{0};
    std::vector<DepthLevel> bids, asks;  // top 8

    // Fills
    struct FillRow {
        uint64_t id; std::string sym; std::string side;
        double qty; double price; uint64_t ts_ns; bool maker{false};
    };
    std::deque<FillRow> fill_log;   // last 200

    // Live (crypto) order book — fractional sizes
    std::vector<std::pair<double,double>> live_bids, live_asks;  // (price, qty)
    bool   live_mode{false};

    // Live position + pnl breakdown (from POS/STAT)
    double pos_net{0}, pos_avg{0}, pos_realized{0}, pos_unreal{0};
    double feed_lat_us{0};
    std::string risk_state{"OK"}, risk_detail;
    uint64_t orders_blocked{0};
    double   maker_ratio{0};

    // Equity
    std::vector<double> equity;
    double initial_cash{500'000.0};
    double final_pnl{0};

    // Metrics
    double sharpe{0}, sortino{0}, calmar{0}, max_dd{0}, vol{0};
    uint64_t fills_total{0}, orders_sent{0};
    int64_t  net_position{0};

    // Progress
    uint64_t tick_num{0};
    uint64_t total_ticks{0};
    bool     running{false};
    bool     done{false};
    bool     kill_armed{false};
    std::string strategy_name;

    // Previous run (for comparison)
    std::vector<double> prev_equity;
    double prev_sharpe{0}, prev_sortino{0}, prev_calmar{0}, prev_max_dd{0}, prev_vol{0};
    double prev_final_pnl{0}, prev_initial_cash{0};
    uint64_t prev_fills_total{0}, prev_orders_sent{0};
    std::string prev_strategy_name;
    bool has_prev{false};

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
    }

    void push_equity(double v) {
        std::lock_guard lk(mu);
        equity.push_back(v);
    }

    void compute_metrics() {
        std::lock_guard lk(mu);
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

        // max drawdown
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
    }

    void save_to_prev() {
        std::lock_guard lk(mu);
        prev_equity      = equity;
        prev_sharpe      = sharpe;
        prev_sortino     = sortino;
        prev_calmar      = calmar;
        prev_max_dd      = max_dd;
        prev_vol         = vol;
        prev_final_pnl   = final_pnl;
        prev_initial_cash= initial_cash;
        prev_fills_total = fills_total;
        prev_orders_sent = orders_sent;
        prev_strategy_name = strategy_name;
        has_prev = true;
    }
};

// ── Simulation Engine (runs in background thread) ────────────────────────────

class SimRunner {
public:
    explicit SimRunner(SimState& state) : state_(state) {}

    void start(const std::string& strategy, int ticks, double sigma,
               int seed, double initial_cash, std::function<void()> done_cb) {
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
        state_.fills_total = state_.orders_sent = 0;
        state_.net_position = 0;
        state_.sharpe = state_.sortino = state_.calmar = state_.max_dd = 0;
        thread_ = std::thread([this, strategy, ticks, sigma, seed, initial_cash, done_cb]{
            run_sim(strategy, ticks, sigma, seed, initial_cash);
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
                  double sigma, int seed, double cash) {
        MatchingEngine eng;
        KillSwitch     ks;
        double         position = 0;
        double         pnl_cash = cash;
        int64_t        tick_n   = 0;

        std::string sym = "AAPL";

        // Callbacks
        eng.on_fill = [&](const Fill& f) {
            double side_sign = (f.aggressor == Side::Buy) ? 1.0 : -1.0;
            position += side_sign * f.qty;
            pnl_cash  -= side_sign * f.qty * f.price;
            SimState::FillRow row;
            row.id    = f.taker_id;
            row.sym   = sym;
            row.side  = (f.aggressor == Side::Buy) ? "BUY" : "SELL";
            row.qty   = f.qty;
            row.price = f.price;
            row.ts_ns = f.ts_ns;
            state_.push_fill(std::move(row));
        };

        eng.on_book_update = [&](const BookSnapshot& snap) {
            auto* lob = eng.book(snap.symbol);
            if (!lob) return;
            state_.set_book(snap.best_bid, snap.best_ask,
                             snap.bid_sz, snap.ask_sz,
                             lob->bid_depth(8), lob->ask_depth(8));
        };

        if (strategy == "mm") run_mm(eng, ks, sym, ticks, sigma, seed,
                                      cash, position, pnl_cash, tick_n);
        else if (strategy == "stat_arb") run_sa(eng, ks, ticks, sigma, seed,
                                                  cash, position, pnl_cash, tick_n);
        else if (strategy == "twap") run_twap(eng, ks, sym, ticks, sigma, seed,
                                               cash, position, pnl_cash, tick_n);

        state_.compute_metrics();
        state_.running = false;
        state_.done    = true;
    }

    // ── Market Making ────────────────────────────────────────────────────────
    void run_mm(MatchingEngine& eng, KillSwitch& ks, const std::string& sym,
                int ticks, double sigma, int seed,
                double cash, double& pos, double& cash_ref, int64_t& tick_n) {
        SyntheticFeed feed({sym, 150.0, 0.0, sigma, 0.01, 2.0, (unsigned)seed});
        double gamma=0.1, kappa=1.5, inv=0;
        uint64_t bid_id=0, ask_id=0, ord_seq=0;
        double last_mid=0;
        const int order_qty=100;
        const int MAX_INV=1000;

        auto next_id = [&]{ return ++ord_seq; };

        auto submit = [&](Side side, double price, uint32_t qty) -> uint64_t {
            Order o; o.id=next_id(); o.symbol=sym; o.side=side;
            o.type=OrdType::Limit; o.price=price; o.qty=qty;
            auto res = eng.submit(o);
            if (!res.fills.empty()) {
                for (auto& f : res.fills) {
                    double sign = (side==Side::Buy)?1:-1;
                    pos      += sign*f.qty;
                    cash_ref -= sign*f.qty*f.price;
                }
            }
            ++state_.orders_sent;
            return o.id;
        };
        auto cancel_id = [&](uint64_t id){ if(id) eng.cancel(id); };

        for (int i=0; i<ticks && !stop_flag_.load(); ++i) {
            auto tick = feed.next_tick();
            double mid = tick.mid();

            if (std::abs(inv) >= MAX_INV) {
                cancel_id(bid_id); cancel_id(ask_id);
                bid_id=ask_id=0;
            } else {
                double T   = std::max(0.01, 1.0-(double)i/ticks);
                double r   = mid - inv*gamma*sigma*sigma*T;
                double spd = std::max(0.005, gamma*sigma*sigma*T/2.0 +
                                             std::log(1.0+gamma/kappa)/gamma);

                bool reprice = (last_mid==0 || std::abs(mid-last_mid) > 0.005);
                if (reprice) {
                    cancel_id(bid_id); cancel_id(ask_id);
                    bid_id = submit(Side::Buy,  std::round((r-spd)*100)/100, order_qty);
                    ask_id = submit(Side::Sell, std::round((r+spd)*100)/100, order_qty);
                    last_mid = mid;
                }
            }

            // Check for fills that changed inventory
            double mark = mid;
            double eq   = cash_ref + pos*mark;
            state_.push_equity(eq);
            state_.tick_num = i+1;
            if ((i%50)==0) state_.compute_metrics();
        }
    }

    // ── Statistical Arbitrage ────────────────────────────────────────────────
    void run_sa(MatchingEngine& eng, KillSwitch& ks,
                int ticks, double sigma, int seed,
                double cash, double& pos, double& cash_ref, int64_t& tick_n) {
        OUSpreadFeed feed("SPY","QQQ", 450.0, 0.08, 0.0, 0.05, sigma, 0.01, 2.0, 1.0, (unsigned)seed);

        const int W=60; std::deque<double> buf;
        std::string mode="flat";
        const int qty=100;
        uint64_t ord_seq=0;
        double pos_a=0,pos_b=0;
        std::unordered_map<std::string,double> mids;

        auto next_id=[&]{return ++ord_seq;};
        auto submit=[&](const std::string& sym,Side side,double price,uint32_t q){
            Order o; o.id=next_id(); o.symbol=sym; o.side=side;
            o.type=OrdType::Market; o.price=price; o.qty=q;
            auto res=eng.submit(o);
            double sign=(side==Side::Buy)?1:-1;
            for(auto& f:res.fills){ cash_ref -= sign*f.qty*f.price; }
            ++state_.orders_sent;
        };

        for (int i=0; i<ticks && !stop_flag_.load(); ++i) {
            auto [ta,tb] = feed.next();
            mids["SPY"]=ta.mid(); mids["QQQ"]=tb.mid();

            double sp = mids["SPY"]-mids["QQQ"];
            buf.push_back(sp);
            if ((int)buf.size()>W) buf.pop_front();
            double z=0;
            if ((int)buf.size()==W) {
                double m=0; for(auto v:buf) m+=v; m/=W;
                double s=0; for(auto v:buf) s+=(v-m)*(v-m); s=std::sqrt(s/W);
                z = s>0?(sp-m)/s:0;
            }

            if (mode=="flat" && (int)buf.size()==W) {
                if (z>2.0) {
                    submit("SPY",Side::Sell,mids["SPY"],qty);
                    submit("QQQ",Side::Buy, mids["QQQ"],qty);
                    pos_a-=qty; pos_b+=qty; mode="short";
                } else if (z<-2.0) {
                    submit("SPY",Side::Buy, mids["SPY"],qty);
                    submit("QQQ",Side::Sell,mids["QQQ"],qty);
                    pos_a+=qty; pos_b-=qty; mode="long";
                }
            } else if (mode=="short" && std::abs(z)<0.3) {
                submit("SPY",Side::Buy, mids["SPY"],qty);
                submit("QQQ",Side::Sell,mids["QQQ"],qty);
                pos_a+=qty; pos_b-=qty; mode="flat";
            } else if (mode=="long" && std::abs(z)<0.3) {
                submit("SPY",Side::Sell,mids["SPY"],qty);
                submit("QQQ",Side::Buy, mids["QQQ"],qty);
                pos_a-=qty; pos_b+=qty; mode="flat";
            }

            double eq = cash_ref + pos_a*mids["SPY"] + pos_b*mids["QQQ"];
            state_.push_equity(eq);
            state_.tick_num = i+1;
            if ((i%50)==0) state_.compute_metrics();
        }
    }

    // ── TWAP ─────────────────────────────────────────────────────────────────
    void run_twap(MatchingEngine& eng, KillSwitch& ks, const std::string& sym,
                  int ticks, double sigma, int seed,
                  double cash, double& pos, double& cash_ref, int64_t& tick_n) {
        SyntheticFeed feed({sym, 200.0, 0.0, sigma, 0.01, 2.0, (unsigned)seed});
        const int total_qty=5000, n_slices=20;
        int slice_ticks = ticks/n_slices;
        int slices_sent=0, filled=0;
        uint64_t ord_seq=0;

        auto next_id=[&]{return ++ord_seq;};
        auto submit=[&](Side side,double price,uint32_t qty){
            Order o; o.id=next_id(); o.symbol=sym; o.side=side;
            o.type=OrdType::Limit; o.price=price; o.qty=qty;
            auto res=eng.submit(o);
            for(auto& f:res.fills){ pos+=f.qty; cash_ref-=f.qty*f.price; filled+=f.qty; }
            ++state_.orders_sent;
        };

        for(int i=0; i<ticks && !stop_flag_.load(); ++i) {
            auto tick = feed.next_tick();
            if(i%slice_ticks==0 && slices_sent<n_slices && filled<total_qty) {
                int remaining = total_qty-filled;
                int qty = (slices_sent==n_slices-1) ? remaining :
                           std::min(total_qty/n_slices, remaining);
                submit(Side::Buy, tick.ask, qty);
                ++slices_sent;
            }
            double eq = cash_ref + pos*tick.mid();
            state_.push_equity(eq);
            state_.tick_num=i+1;
            if((i%50)==0) state_.compute_metrics();
        }
    }

    SimState&            state_;
    std::atomic<bool>    stop_flag_{false};
    std::thread          thread_;
};

// ── Equity Curve Drawing Area ─────────────────────────────────────────────────

class EquityCurve : public Gtk::DrawingArea {
public:
    EquityCurve() {
        set_draw_func(sigc::mem_fun(*this, &EquityCurve::on_draw));
        set_expand(true);
    }

    void set_data(const std::vector<double>& eq, double initial_cash) {
        equity_ = eq; initial_cash_ = initial_cash; queue_draw();
    }
    void set_dark(bool d) { dark_ = d; }

private:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        const double pad_l=60, pad_r=15, pad_t=24, pad_b=36;

        if (dark_) cr->set_source_rgb(0.11, 0.11, 0.12);
        else       cr->set_source_rgb(0.98, 0.98, 0.99);
        cr->paint();

        if (equity_.size() < 2) {
            cr->set_source_rgba(0.5, 0.55, 0.65, 0.7);
            cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                                  Cairo::ToyFontFace::Weight::NORMAL);
            cr->set_font_size(13);
            cr->move_to(w/2 - 110, h/2);
            cr->show_text("Run a strategy to see the equity curve");
            return;
        }

        auto& eq = equity_;
        double mn = *std::min_element(eq.begin(),eq.end());
        double mx = *std::max_element(eq.begin(),eq.end());
        double rng = mx - mn;
        // Only pad when the series is essentially flat; otherwise keep a tight
        // auto-scale so small PnL moves on a large cash base stay visible.
        if (rng < 1e-9) {
            double p = std::max(1.0, std::fabs(mn) * 1e-5);
            mn -= p; mx += p; rng = mx-mn;
        }
        mn -= rng*0.05; mx += rng*0.05; rng = mx-mn;

        double cw = w - pad_l - pad_r;
        double ch = h - pad_t - pad_b;

        auto px = [&](int i) { return pad_l + (double)i/(eq.size()-1)*cw; };
        auto py = [&](double v) { return pad_t + ch - (v-mn)/rng*ch; };

        // Grid lines
        double grid_alpha = dark_ ? 0.12 : 0.18;
        cr->set_line_width(0.5);
        for (int g=0; g<=5; ++g) {
            double yg = pad_t + g*(ch/5.0);
            cr->set_source_rgba(0.5,0.5,0.65, grid_alpha);
            cr->move_to(pad_l, yg); cr->line_to(w-pad_r, yg); cr->stroke();
            double val = mx - g*(rng/5.0);
            if (dark_) cr->set_source_rgba(0.55,0.60,0.70,0.8);
            else       cr->set_source_rgba(0.35,0.38,0.50,0.9);
            cr->set_font_size(9);
            std::ostringstream ss;
            ss << "$" << std::fixed << std::setprecision(0) << val;
            cr->move_to(2, yg+4); cr->show_text(ss.str());
        }

        // Zero line (initial cash)
        if (initial_cash_ >= mn && initial_cash_ <= mx) {
            double y0 = py(initial_cash_);
            if (dark_) cr->set_source_rgba(0.5,0.5,0.65,0.45);
            else       cr->set_source_rgba(0.4,0.4,0.55,0.5);
            cr->set_line_width(1.2);
            std::vector<double> dashes={6,4};
            cr->set_dash(dashes,0);
            cr->move_to(pad_l,y0); cr->line_to(w-pad_r,y0); cr->stroke();
            cr->unset_dash();
        }

        // Filled area under curve
        bool positive_pnl = eq.back() >= initial_cash_;
        cr->move_to(px(0), py(initial_cash_ > mn ? initial_cash_ : mn));
        cr->line_to(px(0), py(eq[0]));
        for (int i=1; i<(int)eq.size(); ++i) cr->line_to(px(i), py(eq[i]));
        cr->line_to(px(eq.size()-1), py(initial_cash_ > mn ? initial_cash_ : mn));
        cr->close_path();
        if (positive_pnl)
            cr->set_source_rgba(0.15,0.6,0.3,0.2);
        else
            cr->set_source_rgba(0.8,0.15,0.15,0.2);
        cr->fill();

        // Equity line
        cr->move_to(px(0), py(eq[0]));
        for (int i=1; i<(int)eq.size(); ++i) cr->line_to(px(i), py(eq[i]));
        cr->set_line_width(1.8);
        if (positive_pnl)
            cr->set_source_rgb(0.2,0.85,0.4);
        else
            cr->set_source_rgb(0.9,0.25,0.25);
        cr->stroke();

        // Axes
        if (dark_) cr->set_source_rgba(0.4,0.42,0.55,0.9);
        else       cr->set_source_rgba(0.55,0.58,0.68,0.9);
        cr->set_line_width(1.0);
        cr->move_to(pad_l,pad_t); cr->line_to(pad_l,h-pad_b);
        cr->line_to(w-pad_r,h-pad_b); cr->stroke();

        // Current value label
        double pnl = eq.back() - initial_cash_;
        bool pos   = pnl >= 0;
        if (dark_) cr->set_source_rgb(pos?0.2:0.95, pos?0.85:0.3, pos?0.4:0.25);
        else       cr->set_source_rgb(pos?0.03:0.82, pos?0.58:0.15, pos?0.38:0.15);
        cr->set_font_size(11);
        cr->select_font_face("Sans", Cairo::ToyFontFace::Slant::NORMAL,
                              Cairo::ToyFontFace::Weight::BOLD);
        std::ostringstream ss;
        ss << "P&L: " << (pos?"+":"") << std::fixed << std::setprecision(2) << pnl
           << "   (" << std::setprecision(2) << pnl/initial_cash_*100 << "%)";
        cr->move_to(pad_l+8, pad_t+16);
        cr->show_text(ss.str());
    }

    std::vector<double> equity_;
    double initial_cash_{500'000.0};
    bool dark_{true};
};

// ── Order Book Depth Chart ────────────────────────────────────────────────────

class DepthChart : public Gtk::DrawingArea {
public:
    DepthChart() {
        set_draw_func(sigc::mem_fun(*this, &DepthChart::on_draw));
        set_expand(true);
        set_size_request(220, -1);
    }

    // C++ engine path: DepthLevel has integer qty
    void update(const std::vector<DepthLevel>& bids, const std::vector<DepthLevel>& asks,
                double bb, double ba) {
        bids_.clear(); asks_.clear();
        for (auto& d : bids) bids_.emplace_back(d.price, (double)d.qty);
        for (auto& d : asks) asks_.emplace_back(d.price, (double)d.qty);
        bb_=bb; ba_=ba; queue_draw();
    }
    // Live crypto path: fractional qty (price, qty) pairs
    void update_levels(const std::vector<std::pair<double,double>>& bids,
                       const std::vector<std::pair<double,double>>& asks,
                       double bb, double ba) {
        bids_=bids; asks_=asks; bb_=bb; ba_=ba; queue_draw();
    }
    void set_dark(bool d) { dark_ = d; }

private:
    static std::string fmt_qty(double q) {
        std::ostringstream s;
        if      (q >= 1000) s << std::fixed << std::setprecision(0) << q;
        else if (q >= 1)    s << std::fixed << std::setprecision(3) << q;
        else                s << std::fixed << std::setprecision(5) << q;
        return s.str();
    }

    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        if (dark_) cr->set_source_rgb(0.11, 0.11, 0.12);
        else       cr->set_source_rgb(0.98, 0.98, 0.99);
        cr->paint();

        if (bids_.empty() && asks_.empty()) {
            if (dark_) cr->set_source_rgba(0.45, 0.50, 0.62, 0.8);
            else       cr->set_source_rgba(0.50, 0.54, 0.68, 0.8);
            cr->set_font_size(11);
            cr->move_to(12, h/2);
            cr->show_text("Awaiting market data…");
            return;
        }

        // Collect all levels
        int n_bid = (int)bids_.size();
        int n_ask = (int)asks_.size();
        int n_total = n_bid + n_ask;
        if (n_total==0) return;

        double max_qty = 1e-12;
        for (auto& d : bids_) max_qty = std::max(max_qty, d.second);
        for (auto& d : asks_) max_qty = std::max(max_qty, d.second);

        double row_h = (double)h / (n_total + 1);
        double bar_max_w = w - 90.0;
        double font_size = std::min(11.0, row_h * 0.6);
        cr->set_font_size(font_size);

        // Color palette — neutral, professional, less saturated
        double ask_bar_r, ask_bar_g, ask_bar_b, ask_bar_a;
        double ask_txt_r, ask_txt_g, ask_txt_b;
        double bid_bar_r, bid_bar_g, bid_bar_b, bid_bar_a;
        double bid_txt_r, bid_txt_g, bid_txt_b;
        double qty_r, qty_g, qty_b;
        double spr_r, spr_g, spr_b;
        if (dark_) {
            // Dark: muted coral red ask, muted teal bid
            ask_bar_r=0.80; ask_bar_g=0.22; ask_bar_b=0.22; ask_bar_a=0.18;
            ask_txt_r=1.00; ask_txt_g=0.45; ask_txt_b=0.45;
            bid_bar_r=0.08; bid_bar_g=0.75; bid_bar_b=0.45; bid_bar_a=0.15;
            bid_txt_r=0.18; bid_txt_g=0.82; bid_txt_b=0.55;
            qty_r=0.55; qty_g=0.55; qty_b=0.65;
            spr_r=0.04; spr_g=0.52; spr_b=1.00;
        } else {
            ask_bar_r=0.90; ask_bar_g=0.20; ask_bar_b=0.20; ask_bar_a=0.08;
            ask_txt_r=0.80; ask_txt_g=0.10; ask_txt_b=0.10;
            bid_bar_r=0.05; bid_bar_g=0.60; bid_bar_b=0.35; bid_bar_a=0.08;
            bid_txt_r=0.02; bid_txt_g=0.50; bid_txt_b=0.28;
            qty_r=0.40; qty_g=0.40; qty_b=0.55;
            spr_r=0.04; spr_g=0.40; spr_b=0.85;
        }

        // Ask levels (top, red) — displayed in reverse (worst→best)
        for (int i=(int)asks_.size()-1; i>=0; --i) {
            auto& d = asks_[i];
            int    row   = (int)asks_.size()-1-i;
            double y     = row * row_h;
            double bar_w = d.second / max_qty * bar_max_w;

            cr->set_source_rgba(ask_bar_r,ask_bar_g,ask_bar_b,ask_bar_a);
            cr->rectangle(0, y, bar_w, row_h-1); cr->fill();
            cr->set_source_rgb(ask_txt_r,ask_txt_g,ask_txt_b);
            cr->move_to(5, y+row_h*0.72);
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << d.first;
            cr->show_text(ss.str());
            cr->set_source_rgba(qty_r,qty_g,qty_b,0.9);
            cr->move_to(w-70, y+row_h*0.72);
            cr->show_text(fmt_qty(d.second));
        }

        // Spread row
        double y_spread = n_ask * row_h;
        cr->set_source_rgba(spr_r,spr_g,spr_b, dark_?0.18:0.10);
        cr->rectangle(0, y_spread, w, row_h-1); cr->fill();
        cr->set_source_rgb(spr_r,spr_g,spr_b);
        cr->set_font_size(std::min(9.5, font_size));
        std::ostringstream sss;
        sss << "▲ " << std::fixed << std::setprecision(4) << (ba_-bb_);
        cr->move_to(5, y_spread+row_h*0.72);
        cr->show_text(sss.str());

        // Bid levels (bottom, green) — best first
        for (int i=0; i<(int)bids_.size(); ++i) {
            auto& d = bids_[i];
            double y     = (n_ask+1+i) * row_h;
            double bar_w = d.second / max_qty * bar_max_w;

            cr->set_source_rgba(bid_bar_r,bid_bar_g,bid_bar_b,bid_bar_a);
            cr->rectangle(0, y, bar_w, row_h-1); cr->fill();
            cr->set_source_rgb(bid_txt_r,bid_txt_g,bid_txt_b);
            cr->move_to(5, y+row_h*0.72);
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << d.first;
            cr->show_text(ss.str());
            cr->set_source_rgba(qty_r,qty_g,qty_b,0.9);
            cr->move_to(w-70, y+row_h*0.72);
            cr->show_text(fmt_qty(d.second));
        }
    }

    std::vector<std::pair<double,double>> bids_, asks_;   // (price, qty)
    double bb_{0}, ba_{0};
    bool dark_{true};
};

// ── Comparison Panel (side-by-side metrics) ──────────────────────────────────

class ComparisonPanel : public Gtk::Box {
public:
    ComparisonPanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 0) {
        set_margin_start(12); set_margin_end(12); set_margin_top(12); set_margin_bottom(12);

        // Header
        auto hdr = Gtk::make_managed<Gtk::Label>();
        hdr->set_markup("<b>Strategy Comparison</b>");
        hdr->set_halign(Gtk::Align::START);
        hdr->set_margin_bottom(8);
        append(*hdr);

        // Metrics grid: 3 columns (metric name | current | previous)
        grid_ = Gtk::make_managed<Gtk::Grid>();
        grid_->set_row_spacing(6);
        grid_->set_column_spacing(12);

        auto mk_header = [](const char* t) {
            auto l = Gtk::make_managed<Gtk::Label>(t);
            l->get_style_context()->add_class("metric-key");
            l->set_markup(std::string("<b>") + t + "</b>");
            return l;
        };
        grid_->attach(*mk_header("Metric"),    0, 0);
        grid_->attach(*mk_header("Current"),   1, 0);
        grid_->attach(*mk_header("Previous"),  2, 0);

        auto add_row = [this](int row, const char* name, Gtk::Label*& cur, Gtk::Label*& prev) {
            auto l = Gtk::make_managed<Gtk::Label>(name);
            l->get_style_context()->add_class("metric-key");
            l->set_halign(Gtk::Align::START);
            grid_->attach(*l, 0, row);
            cur = Gtk::make_managed<Gtk::Label>("—");
            cur->get_style_context()->add_class("metric-val");
            grid_->attach(*cur, 1, row);
            prev = Gtk::make_managed<Gtk::Label>("—");
            prev->get_style_context()->add_class("metric-val");
            grid_->attach(*prev, 2, row);
        };

        add_row(1, "Strategy",    lbl_cur_strat_,   lbl_prev_strat_);
        add_row(2, "Sharpe",      lbl_cur_sharpe_,  lbl_prev_sharpe_);
        add_row(3, "Sortino",     lbl_cur_sortino_, lbl_prev_sortino_);
        add_row(4, "Max DD",      lbl_cur_dd_,      lbl_prev_dd_);
        add_row(5, "Volatility",  lbl_cur_vol_,     lbl_prev_vol_);
        add_row(6, "PnL",         lbl_cur_pnl_,     lbl_prev_pnl_);
        add_row(7, "Realized",    lbl_cur_realized_, lbl_prev_realized_);
        add_row(8, "Fills",       lbl_cur_fills_,   lbl_prev_fills_);
        add_row(9, "Orders",      lbl_cur_orders_,  lbl_prev_orders_);

        auto sw = Gtk::make_managed<Gtk::ScrolledWindow>();
        sw->set_child(*grid_);
        sw->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        append(*sw);

        set_expand(true);
    }

    void update(const SimState& st) {
        lbl_cur_strat_->set_text(st.strategy_name.empty() ? "—" : st.strategy_name);
        lbl_cur_sharpe_->set_text(fmt(st.sharpe, 3));
        lbl_cur_sortino_->set_text(fmt(st.sortino, 3));
        lbl_cur_dd_->set_text(fmt(st.max_dd * 100, 2) + "%");
        lbl_cur_vol_->set_text(fmt(st.vol * 100, 3) + "%");
        lbl_cur_pnl_->set_text(fmt_pnl(st.final_pnl));
        lbl_cur_realized_->set_text(fmt_pnl(st.final_pnl));  // approx for now
        lbl_cur_fills_->set_text(std::to_string(st.fills_total));
        lbl_cur_orders_->set_text(std::to_string(st.orders_sent));

        if (st.has_prev) {
            lbl_prev_strat_->set_text(st.prev_strategy_name);
            lbl_prev_sharpe_->set_text(fmt(st.prev_sharpe, 3));
            lbl_prev_sortino_->set_text(fmt(st.prev_sortino, 3));
            lbl_prev_dd_->set_text(fmt(st.prev_max_dd * 100, 2) + "%");
            lbl_prev_vol_->set_text(fmt(st.prev_vol * 100, 3) + "%");
            lbl_prev_pnl_->set_text(fmt_pnl(st.prev_final_pnl));
            lbl_prev_realized_->set_text(fmt_pnl(st.prev_final_pnl));
            lbl_prev_fills_->set_text(std::to_string(st.prev_fills_total));
            lbl_prev_orders_->set_text(std::to_string(st.prev_orders_sent));
        } else {
            lbl_prev_strat_->set_text("—");
            lbl_prev_sharpe_->set_text("—");
            lbl_prev_sortino_->set_text("—");
            lbl_prev_dd_->set_text("—");
            lbl_prev_vol_->set_text("—");
            lbl_prev_pnl_->set_text("—");
            lbl_prev_realized_->set_text("—");
            lbl_prev_fills_->set_text("—");
            lbl_prev_orders_->set_text("—");
        }
    }

private:
    std::string fmt(double v, int dp) {
        std::ostringstream s;
        s << std::fixed << std::setprecision(dp) << v;
        return s.str();
    }

    std::string fmt_pnl(double v) {
        std::ostringstream s;
        s << (v >= 0 ? "+" : "") << "$" << std::fixed << std::setprecision(2) << v;
        return s.str();
    }

    Gtk::Grid* grid_;
    Gtk::Label *lbl_cur_strat_, *lbl_prev_strat_;
    Gtk::Label *lbl_cur_sharpe_, *lbl_prev_sharpe_;
    Gtk::Label *lbl_cur_sortino_, *lbl_prev_sortino_;
    Gtk::Label *lbl_cur_dd_, *lbl_prev_dd_;
    Gtk::Label *lbl_cur_vol_, *lbl_prev_vol_;
    Gtk::Label *lbl_cur_pnl_, *lbl_prev_pnl_;
    Gtk::Label *lbl_cur_realized_, *lbl_prev_realized_;
    Gtk::Label *lbl_cur_fills_, *lbl_prev_fills_;
    Gtk::Label *lbl_cur_orders_, *lbl_prev_orders_;
};

// ── Embedded Console / Terminal ───────────────────────────────────────────────
//
// Not a full PTY (no vte dependency) — a command runner: type a shell command,
// it runs from the repo root in a background thread, stdout+stderr stream live
// into the view. Up/Down navigate history. Stop SIGTERMs the running command.
// Quick buttons prefill common QuantSim commands.

class ConsolePanel : public Gtk::Box {
public:
    ConsolePanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 0) {
        get_style_context()->add_class("console-panel");

        // Quick-command bar
        auto qbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        qbar->set_margin_start(8); qbar->set_margin_end(8);
        qbar->set_margin_top(8);   qbar->set_margin_bottom(4);
        auto add_quick = [&](const char* label, const std::string& cmd) {
            auto b = Gtk::make_managed<Gtk::Button>(label);
            b->get_style_context()->add_class("quick-btn");
            b->signal_clicked().connect([this, cmd]{ input_.set_text(cmd); input_.grab_focus(); });
            qbar->append(*b);
        };
        add_quick("Live MM · BTC",
            "PYTHONPATH=python python3 -u -m quantsim.live.run --symbol BTCUSDT --strategy mm --no-stream --duration 30");
        add_quick("Live Momentum · ETH",
            "PYTHONPATH=python python3 -u -m quantsim.live.run --symbol ETHUSDT --strategy momentum --no-stream --duration 30");
        add_quick("Live Tests",
            "PYTHONPATH=python python3 tests/test_live.py 2>&1 || true");
        add_quick("git status", "git status -s");
        append(*qbar);

        // Output view
        out_buf_ = Gtk::TextBuffer::create();
        out_view_.set_buffer(out_buf_);
        out_view_.set_editable(false);
        out_view_.set_monospace(true);
        out_view_.set_cursor_visible(false);
        out_view_.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
        out_view_.get_style_context()->add_class("console-out");
        out_scroll_.set_child(out_view_);
        out_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::ALWAYS);
        out_scroll_.set_expand(true);
        out_scroll_.set_margin_start(8); out_scroll_.set_margin_end(8);
        append(out_scroll_);

        // Input row
        auto irow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        irow->set_margin_start(8); irow->set_margin_end(8);
        irow->set_margin_top(4);   irow->set_margin_bottom(8);
        auto prompt = Gtk::make_managed<Gtk::Label>("❯");
        prompt->get_style_context()->add_class("console-prompt");
        input_.set_hexpand(true);
        input_.set_placeholder_text("type a command — e.g. python3 -m quantsim.live.run --symbol BTCUSDT --strategy mm --no-stream");
        input_.get_style_context()->add_class("console-input");
        run_btn_.set_label("Run");   run_btn_.get_style_context()->add_class("run-btn");
        stop_btn_.set_label("Stop");  stop_btn_.get_style_context()->add_class("stop-btn");
        clear_btn_.set_label("Clear");
        stop_btn_.set_sensitive(false);
        irow->append(*prompt);
        irow->append(input_);
        irow->append(run_btn_);
        irow->append(stop_btn_);
        irow->append(clear_btn_);
        append(*irow);

        // Wiring
        input_.signal_activate().connect([this]{ run_current(); });
        run_btn_.signal_clicked().connect([this]{ run_current(); });
        stop_btn_.signal_clicked().connect([this]{ stop(); });
        clear_btn_.signal_clicked().connect([this]{ out_buf_->set_text(""); });

        dispatcher_.connect([this]{ drain(); });

        // Up/Down history navigation
        auto key = Gtk::EventControllerKey::create();
        key->signal_key_pressed().connect(
            [this](guint keyval, guint, Gdk::ModifierType) -> bool {
                if (keyval == GDK_KEY_Up)   { history_prev(); return true; }
                if (keyval == GDK_KEY_Down) { history_next(); return true; }
                return false;
            }, false);
        input_.add_controller(key);

        append_text("QuantSim console — commands run from the repo root.\n"
                    "Try the quick buttons above, or run any shell command.\n\n");
    }

    ~ConsolePanel() override { stop(); if (worker_.joinable()) worker_.join(); }

    void set_dark(bool) {}   // styling handled by global CSS

private:
    void run_current() {
        std::string cmd = input_.get_text();
        // trim
        auto a = cmd.find_first_not_of(" \t");
        if (a == std::string::npos) return;
        cmd = cmd.substr(a, cmd.find_last_not_of(" \t") - a + 1);
        if (cmd.empty()) return;
        if (running_.load()) {
            append_text("[busy] a command is already running — Stop it first.\n");
            return;
        }
        history_.push_back(cmd);
        hist_idx_ = (int)history_.size();
        input_.set_text("");
        append_text("\n❯ " + cmd + "\n");

        running_.store(true);
        run_btn_.set_sensitive(false);
        stop_btn_.set_sensitive(true);

        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this, cmd]{
            // Background the command so we can capture its PID for Stop.
            std::string full = "{ " + cmd + " ; } 2>&1 & echo $! > .quantsim_console.pid; wait";
            FILE* p = popen(full.c_str(), "r");
            if (!p) {
                { std::lock_guard lk(mu_); pending_ += "[error] failed to launch\n"; }
                running_.store(false); dispatcher_.emit(); return;
            }
            char buf[1024];
            while (fgets(buf, sizeof(buf), p)) {
                { std::lock_guard lk(mu_); pending_ += buf; }
                dispatcher_.emit();
            }
            pclose(p);
            std::remove(".quantsim_console.pid");
            { std::lock_guard lk(mu_); pending_ += "\n[done]\n"; }
            running_.store(false);
            dispatcher_.emit();
        });
    }

    void stop() {
        std::ifstream pf(".quantsim_console.pid");
        if (pf) {
            long pid = 0; pf >> pid;
            if (pid > 0) ::kill((pid_t)pid, SIGTERM);
        }
    }

    void drain() {
        std::string chunk;
        { std::lock_guard lk(mu_); chunk.swap(pending_); }
        if (!chunk.empty()) append_text(chunk);
        if (!running_.load()) {
            run_btn_.set_sensitive(true);
            stop_btn_.set_sensitive(false);
        }
    }

    void append_text(const std::string& s) {
        out_buf_->insert(out_buf_->end(), s);
        auto mark = out_buf_->create_mark(out_buf_->end(), false);
        out_view_.scroll_to(mark);
    }

    void history_prev() {
        if (history_.empty()) return;
        if (hist_idx_ > 0) --hist_idx_;
        input_.set_text(history_[hist_idx_]);
        input_.set_position(-1);
    }
    void history_next() {
        if (history_.empty()) return;
        if (hist_idx_ < (int)history_.size() - 1) {
            ++hist_idx_;
            input_.set_text(history_[hist_idx_]);
        } else {
            hist_idx_ = (int)history_.size();
            input_.set_text("");
        }
        input_.set_position(-1);
    }

    Gtk::ScrolledWindow out_scroll_;
    Gtk::TextView       out_view_;
    Glib::RefPtr<Gtk::TextBuffer> out_buf_;
    Gtk::Entry          input_;
    Gtk::Button         run_btn_, stop_btn_, clear_btn_;

    Glib::Dispatcher    dispatcher_;
    std::thread         worker_;
    std::atomic<bool>   running_{false};
    std::mutex          mu_;
    std::string         pending_;

    std::vector<std::string> history_;
    int hist_idx_{0};
};

// ── Main Window ───────────────────────────────────────────────────────────────

class QuantSimWindow : public Gtk::Window {
public:
    QuantSimWindow()
        : runner_(state_)
    {
        set_title("QuantSim — HFT Simulation Platform");
        set_default_size(1360, 880);

        build_ui();
        setup_dispatcher();
        apply_css();

        // Sync Cairo-drawn widgets to the active theme (light by default)
        equity_curve_.set_dark(dark_mode_);
        depth_chart_.set_dark(dark_mode_);
        editor_panel_.set_dark(dark_mode_);
    }

    ~QuantSimWindow() { runner_.stop(); }

private:
    // ── UI Construction ───────────────────────────────────────────────────────

    void build_ui() {
        auto root_paned = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
        root_paned->set_wide_handle(true);

        // ── Left sidebar ──────────────────────────────────────────────────────
        auto left_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        left_box->get_style_context()->add_class("sidebar");
        left_box->set_size_request(258, -1);

        // Header row (title + theme toggle)
        auto header_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        auto title_col  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        title_col->set_hexpand(true);

        auto title_lbl = Gtk::make_managed<Gtk::Label>("QuantSim");
        title_lbl->get_style_context()->add_class("sidebar-title");
        title_lbl->set_xalign(0);
        auto sub_lbl = Gtk::make_managed<Gtk::Label>("HFT Simulation Platform");
        sub_lbl->get_style_context()->add_class("sidebar-subtitle");
        sub_lbl->set_xalign(0);
        title_col->append(*title_lbl);
        title_col->append(*sub_lbl);

        theme_btn_.set_label(dark_mode_ ? "Light" : "Dark");
        theme_btn_.get_style_context()->add_class("theme-toggle");
        theme_btn_.set_valign(Gtk::Align::CENTER);
        theme_btn_.set_margin_end(10);
        theme_btn_.set_margin_top(8);

        header_box->append(*title_col);
        header_box->append(theme_btn_);
        left_box->append(*header_box);

        auto make_section = [](const char* text) {
            auto l = Gtk::make_managed<Gtk::Label>(text);
            l->get_style_context()->add_class("section-label");
            l->set_xalign(0);
            return l;
        };

        // ── Strategy ──────────────────────────────────────────────────────────
        left_box->append(*Gtk::make_managed<Gtk::Separator>());
        left_box->append(*make_section("STRATEGY"));
        strategy_box_.append("mm",       "Market Making (Avellaneda-Stoikov)");
        strategy_box_.append("momentum", "Order-Flow Momentum (crypto)");
        strategy_box_.append("meanrev",  "Mean Reversion (crypto)");
        strategy_box_.append("stat_arb", "Statistical Arbitrage (OU Spread)");
        strategy_box_.append("twap",     "TWAP Execution");
        strategy_box_.set_active_id("mm");
        strategy_box_.set_margin_start(10); strategy_box_.set_margin_end(10);
        strategy_box_.set_margin_bottom(4);
        left_box->append(strategy_box_);

        // ── Data Source ───────────────────────────────────────────────────────
        left_box->append(*Gtk::make_managed<Gtk::Separator>());
        left_box->append(*make_section("DATA SOURCE"));

        source_box_.append("synthetic", "Synthetic (GBM)");
        source_box_.append("crypto",    "Crypto — Binance Live");
        source_box_.append("yfinance",  "Yahoo Finance (free)");
        source_box_.append("alpaca",    "Alpaca Markets");
        source_box_.set_active_id("synthetic");
        source_box_.set_margin_start(10); source_box_.set_margin_end(10);
        source_box_.set_margin_bottom(4);
        left_box->append(source_box_);

        // Real-data params (symbol / period / interval) — hidden when synthetic
        real_data_box_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        real_data_box_->set_margin_start(10); real_data_box_->set_margin_end(10);
        real_data_box_->set_margin_bottom(4);

        auto ds_grid = Gtk::make_managed<Gtk::Grid>();
        ds_grid->set_row_spacing(6);
        ds_grid->set_column_spacing(8);

        auto mk_ds_label = [](const char* t) {
            auto l = Gtk::make_managed<Gtk::Label>(t);
            l->set_xalign(0);
            l->get_style_context()->add_class("metric-key");
            return l;
        };

        symbol_entry_.set_text("AAPL");
        symbol_entry_.set_hexpand(true);
        symbol_entry_.set_placeholder_text("e.g. NVDA, SPY");

        period_box_.append("5d",  "5 days");
        period_box_.append("1mo", "1 month");
        period_box_.append("3mo", "3 months");
        period_box_.append("6mo", "6 months");
        period_box_.append("1y",  "1 year");
        period_box_.append("2y",  "2 years");
        period_box_.set_active_id("1y");
        period_box_.set_hexpand(true);

        interval_box_.append("1m",  "1 minute");
        interval_box_.append("5m",  "5 minutes");
        interval_box_.append("15m", "15 minutes");
        interval_box_.append("1h",  "1 hour");
        interval_box_.append("1d",  "1 day");
        interval_box_.set_active_id("1d");
        interval_box_.set_hexpand(true);

        ds_period_lbl_   = mk_ds_label("Period");
        ds_interval_lbl_ = mk_ds_label("Interval");
        ds_grid->attach(*mk_ds_label("Symbol"),   0, 0, 1, 1);
        ds_grid->attach(symbol_entry_,            1, 0, 1, 1);
        ds_grid->attach(*ds_period_lbl_,          0, 1, 1, 1);
        ds_grid->attach(period_box_,              1, 1, 1, 1);
        ds_grid->attach(*ds_interval_lbl_,        0, 2, 1, 1);
        ds_grid->attach(interval_box_,            1, 2, 1, 1);

        real_data_box_->append(*ds_grid);
        real_data_box_->set_visible(false);
        left_box->append(*real_data_box_);

        // Show/hide real-data params when source changes
        source_box_.signal_changed().connect([this]{
            std::string sid = source_box_.get_active_id();
            bool is_real = (sid != "synthetic");
            real_data_box_->set_visible(is_real);
            // Period/Interval only apply to historical equity feeds, not live crypto
            bool show_pi = is_real && sid != "crypto";
            ds_period_lbl_->set_visible(show_pi);
            period_box_.set_visible(show_pi);
            ds_interval_lbl_->set_visible(show_pi);
            interval_box_.set_visible(show_pi);
            if (sid == "crypto") {
                std::string sym = symbol_entry_.get_text();
                if (sym.size() < 5 || sym.rfind("USDT") == std::string::npos)
                    symbol_entry_.set_text("BTCUSDT");
                strategy_box_.set_active_id("mm");
            } else if (sid != "synthetic") {
                std::string sym = symbol_entry_.get_text();
                if (sym.rfind("USDT") != std::string::npos)
                    symbol_entry_.set_text("AAPL");
            }
        });

        // ── Parameters ────────────────────────────────────────────────────────
        auto params_frame = Gtk::make_managed<Gtk::Frame>("Parameters");
        auto params_grid  = Gtk::make_managed<Gtk::Grid>();
        params_grid->set_row_spacing(8);
        params_grid->set_column_spacing(10);
        params_grid->set_margin_start(10); params_grid->set_margin_end(10);
        params_grid->set_margin_top(8);    params_grid->set_margin_bottom(8);

        auto add_param = [&](int row, const char* lbl_text, Gtk::Widget& widget) {
            auto l = Gtk::make_managed<Gtk::Label>(lbl_text);
            l->set_xalign(0);
            l->get_style_context()->add_class("metric-key");
            params_grid->attach(*l,     0, row, 1, 1);
            params_grid->attach(widget, 1, row, 1, 1);
        };

        ticks_spin_.set_adjustment(Gtk::Adjustment::create(10000,100,200000,100,1000));
        ticks_spin_.set_digits(0); ticks_spin_.set_hexpand(true);
        sigma_spin_.set_adjustment(Gtk::Adjustment::create(0.0008,0.0001,0.01,0.0001,0.001));
        sigma_spin_.set_digits(4);
        seed_spin_.set_adjustment(Gtk::Adjustment::create(42,1,9999,1,10));
        seed_spin_.set_digits(0);
        cash_spin_.set_adjustment(Gtk::Adjustment::create(500000,10000,10000000,10000,100000));
        cash_spin_.set_digits(0);

        add_param(0, "Ticks",         ticks_spin_);
        add_param(1, "Volatility σ",  sigma_spin_);
        add_param(2, "Seed",          seed_spin_);
        add_param(3, "Initial Cash $",cash_spin_);
        params_frame->set_child(*params_grid);
        left_box->append(*params_frame);

        // Speed slider
        auto speed_frame = Gtk::make_managed<Gtk::Frame>("Sim Speed (µs delay / tick)");
        auto speed_box   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        speed_box->set_margin_start(10); speed_box->set_margin_end(10);
        speed_box->set_margin_top(6);    speed_box->set_margin_bottom(6);
        speed_scale_.set_range(0,2000);
        speed_scale_.set_value(0);
        speed_scale_.set_digits(0);
        speed_scale_.set_hexpand(true);
        speed_scale_.set_draw_value(true);
        speed_scale_.set_value_pos(Gtk::PositionType::RIGHT);
        speed_box->append(speed_scale_);
        speed_frame->set_child(*speed_box);
        left_box->append(*speed_frame);

        // ── Run / Stop ────────────────────────────────────────────────────────
        auto btn_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        btn_box->set_margin_start(10); btn_box->set_margin_end(10);
        btn_box->set_margin_top(4);    btn_box->set_margin_bottom(4);
        run_btn_.set_label("▶  Run");
        stop_btn_.set_label("■  Stop");
        stop_btn_.set_sensitive(false);
        run_btn_.get_style_context()->add_class("run-btn");
        stop_btn_.get_style_context()->add_class("stop-btn");
        run_btn_.set_hexpand(true);
        stop_btn_.set_hexpand(true);
        btn_box->append(run_btn_);
        btn_box->append(stop_btn_);
        left_box->append(*btn_box);

        progress_bar_.set_show_text(true);
        progress_bar_.set_text("Ready");
        progress_bar_.set_margin_start(10); progress_bar_.set_margin_end(10);
        progress_bar_.set_margin_bottom(4);
        left_box->append(progress_bar_);

        // ── Live Metrics ──────────────────────────────────────────────────────
        left_box->append(*Gtk::make_managed<Gtk::Separator>());
        left_box->append(*make_section("LIVE METRICS"));

        auto met_frame = Gtk::make_managed<Gtk::Frame>();
        auto met_grid  = Gtk::make_managed<Gtk::Grid>();
        met_grid->set_row_spacing(5);
        met_grid->set_column_spacing(12);
        met_grid->set_margin_start(10); met_grid->set_margin_end(10);
        met_grid->set_margin_top(6);    met_grid->set_margin_bottom(6);

        auto add_metric = [&](int row, const char* lbl_text, Gtk::Label& val_lbl) {
            auto l = Gtk::make_managed<Gtk::Label>(lbl_text);
            l->set_xalign(0);
            l->get_style_context()->add_class("metric-key");
            val_lbl.set_xalign(1); val_lbl.set_hexpand(true);
            val_lbl.set_text("—");
            val_lbl.get_style_context()->add_class("metric-val");
            met_grid->attach(*l,      0, row, 1, 1);
            met_grid->attach(val_lbl, 1, row, 1, 1);
        };

        add_metric(0, "Sharpe",       lbl_sharpe_);
        add_metric(1, "Sortino",      lbl_sortino_);
        add_metric(2, "Max Drawdown", lbl_dd_);
        add_metric(3, "Volatility",   lbl_vol_);
        add_metric(4, "Fills",        lbl_fills_);
        add_metric(5, "Orders Sent",  lbl_orders_);
        add_metric(6, "P&L",          lbl_pnl_);
        add_metric(7, "Ticks",        lbl_ticks_);
        add_metric(8, "Position",     lbl_pos_);
        add_metric(9, "Feed Latency", lbl_lat_);
        met_frame->set_child(*met_grid);
        left_box->append(*met_frame);

        // Kill switch badge
        kill_label_.get_style_context()->add_class("ks-ok");
        kill_label_.set_text("● KILL SWITCH: OK");
        kill_label_.set_xalign(0);
        left_box->append(kill_label_);

        // ── Log ───────────────────────────────────────────────────────────────
        // No vexpand spacer here: this whole box lives inside a vertical
        // ScrolledWindow, and a vexpand child makes it demand infinite height,
        // which collapses/glitches the log. Let content pack naturally + scroll.
        left_box->append(*Gtk::make_managed<Gtk::Separator>());
        left_box->append(*make_section("LOG"));
        log_view_.set_buffer(log_buf_ = Gtk::TextBuffer::create());
        log_view_.set_editable(false);
        log_view_.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
        log_view_.set_monospace(true);
        log_view_.set_size_request(-1, 100);
        log_scroll_.set_child(log_view_);
        log_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
        log_scroll_.set_margin_start(4); log_scroll_.set_margin_end(4);
        log_scroll_.set_margin_bottom(6);
        left_box->append(log_scroll_);

        // Wrap sidebar in ScrolledWindow so content never escapes the window
        auto sidebar_sw = Gtk::make_managed<Gtk::ScrolledWindow>();
        sidebar_sw->set_child(*left_box);
        sidebar_sw->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        sidebar_sw->set_vexpand(true);
        sidebar_sw->set_size_request(262, -1);

        root_paned->set_start_child(*sidebar_sw);
        root_paned->set_resize_start_child(false);

        // ── Right: Notebook ───────────────────────────────────────────────────
        auto notebook = Gtk::make_managed<Gtk::Notebook>();
        notebook->set_tab_pos(Gtk::PositionType::TOP);
        notebook->set_expand(true);

        // Tab 1: Dashboard
        auto dash_paned = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);

        depth_chart_.set_size_request(260, -1);
        auto depth_frame = Gtk::make_managed<Gtk::Frame>("Order Book Depth");
        depth_frame->set_child(depth_chart_);
        depth_frame->set_margin_start(6); depth_frame->set_margin_top(6);
        depth_frame->set_margin_bottom(6);
        dash_paned->set_start_child(*depth_frame);
        dash_paned->set_resize_start_child(false);
        dash_paned->set_position(270);

        auto fill_frame = Gtk::make_managed<Gtk::Frame>("Fill Log");
        build_fill_table();
        fill_scroll_.set_child(*fill_tree_view_);
        fill_scroll_.set_expand(true);
        fill_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::ALWAYS);
        fill_frame->set_child(fill_scroll_);
        fill_frame->set_margin_end(6); fill_frame->set_margin_top(6);
        fill_frame->set_margin_bottom(6); fill_frame->set_margin_start(4);
        dash_paned->set_end_child(*fill_frame);

        // Helper: icon + label tab widget
        auto make_tab_label = [](const char* icon_name, const char* text) {
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            box->set_margin_start(2); box->set_margin_end(2);
            auto img = Gtk::make_managed<Gtk::Image>();
            img->set_from_icon_name(icon_name);
            img->set_icon_size(Gtk::IconSize::NORMAL);
            auto lbl = Gtk::make_managed<Gtk::Label>(text);
            box->append(*img);
            box->append(*lbl);
            return box;
        };

        notebook->append_page(*dash_paned,
            *make_tab_label("utilities-system-monitor-symbolic", "Dashboard"));

        // Tab 2: Equity Curve
        auto equity_frame = Gtk::make_managed<Gtk::Frame>("Equity Curve");
        equity_frame->set_child(equity_curve_);
        equity_frame->set_margin_start(6); equity_frame->set_margin_end(6);
        equity_frame->set_margin_top(6);   equity_frame->set_margin_bottom(6);
        notebook->append_page(*equity_frame,
            *make_tab_label("office-chart-line-symbolic", "Equity"));

        // Tab 3: Results
        results_view_.set_buffer(results_buf_ = Gtk::TextBuffer::create());
        results_view_.set_editable(false);
        results_view_.set_monospace(true);
        results_view_.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
        results_scroll_.set_child(results_view_);
        results_scroll_.set_expand(true);
        results_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::ALWAYS);
        notebook->append_page(results_scroll_,
            *make_tab_label("document-properties-symbolic", "Results"));

        // Tab 4: Strategy Editor
        notebook->append_page(editor_panel_,
            *make_tab_label("document-edit-symbolic", "Editor"));

        // Tab 5: Terminal / Console
        notebook->append_page(console_panel_,
            *make_tab_label("utilities-terminal-symbolic", "Terminal"));

        // Tab 6: Comparison
        notebook->append_page(comparison_panel_,
            *make_tab_label("view-switch-symbolic", "Compare"));

        root_paned->set_end_child(*notebook);
        set_child(*root_paned);
        root_paned->set_position(265);

        run_btn_.signal_clicked().connect(sigc::mem_fun(*this,&QuantSimWindow::on_run));
        stop_btn_.signal_clicked().connect(sigc::mem_fun(*this,&QuantSimWindow::on_stop));
        theme_btn_.signal_clicked().connect(sigc::mem_fun(*this,&QuantSimWindow::toggle_theme));
    }

    void build_fill_table() {
        fill_store_ = Gtk::ListStore::create(fill_cols_);
        fill_tree_view_ = Gtk::make_managed<Gtk::TreeView>(fill_store_);
        fill_tree_view_->set_headers_visible(true);

        auto add_col = [&](const char* title, Gtk::TreeModelColumn<Glib::ustring>& col){
            auto* renderer = Gtk::make_managed<Gtk::CellRendererText>();
            auto* tvcol    = Gtk::make_managed<Gtk::TreeViewColumn>(title, *renderer);
            tvcol->add_attribute(renderer->property_text(), col);
            tvcol->set_resizable(true);
            tvcol->set_min_width(60);
            fill_tree_view_->append_column(*tvcol);
        };
        add_col("ID",    fill_cols_.col_id);
        add_col("Symbol",fill_cols_.col_sym);
        add_col("Side",  fill_cols_.col_side);
        add_col("Qty",   fill_cols_.col_qty);
        add_col("Price", fill_cols_.col_price);
        fill_tree_view_->set_enable_search(false);
    }

    void setup_dispatcher() {
        dispatcher_.connect([this]{
            refresh_ui();
        });
    }

    static constexpr const char* DARK_CSS = R"css(
/* ── Base ─────────────────────────────────────────────────────────────────── */
window,
.sidebar,
.view { background-color: #1c1c1e; color: #f2f2f7; }

/* Kill white in notebook content / scrolled areas */
notebook > stack,
notebook stack,
stack,
viewport,
scrolledwindow,
scrolledwindow > viewport,
scrolledwindow > viewport > * { background-color: #1c1c1e; color: #f2f2f7; }

/* Kill white in popovers (ComboBox dropdown) */
popover,
popover > contents,
popover contents { background-color: #2c2c2e; color: #f2f2f7; border: 1px solid #48484a; border-radius: 10px; }

/* ── Sidebar ── */
.sidebar { border-right: 1px solid #2c2c2e; }
.sidebar-title {
    font-size: 15pt; font-weight: 800;
    color: #f2f2f7; letter-spacing: -0.3px;
    padding: 14px 14px 1px 14px;
}
.sidebar-subtitle {
    font-size: 7.5pt; color: #636366;
    font-weight: 600; letter-spacing: 1.5px;
    text-transform: uppercase; padding: 0 14px 12px 14px;
}
.section-label {
    font-size: 7pt; font-weight: 700; letter-spacing: 2px;
    color: #48484a; text-transform: uppercase; padding: 10px 14px 4px 14px;
}

/* ── Cards ── */
frame {
    border: 1px solid #2c2c2e; border-radius: 10px;
    background-color: #2c2c2e; margin: 2px 8px;
}
frame > label {
    color: #aeaeb2; font-size: 8.5pt;
    font-weight: 600; padding: 0 6px;
}
separator { background-color: #2c2c2e; min-height: 1px; margin: 4px 8px; }

/* ── Buttons ── */
button {
    background-color: #3a3a3c; color: #f2f2f7;
    border: 1px solid #48484a; border-radius: 8px;
    padding: 6px 14px; font-size: 9.5pt;
}
button:hover  { background-color: #48484a; }
button:active { background-color: #2c2c2e; }
button:disabled { color: #48484a; border-color: #2c2c2e; }

.run-btn {
    background: linear-gradient(160deg, #0a84ff 0%, #0066cc 100%);
    color: #fff; border: none; border-radius: 8px;
    font-weight: 700; font-size: 10pt;
}
.run-btn:hover  { background: linear-gradient(160deg, #2196ff 0%, #0a84ff 100%); }
.run-btn:disabled { background: #2c2c2e; color: #48484a; }

.stop-btn {
    background: linear-gradient(160deg, #ff453a 0%, #cc2318 100%);
    color: #fff; border: none; border-radius: 8px;
    font-weight: 700; font-size: 10pt;
}
.stop-btn:hover  { background: linear-gradient(160deg, #ff6961 0%, #ff453a 100%); }
.stop-btn:disabled { background: #2c2c2e; color: #48484a; }

/* ── Inputs ── */
spinbutton {
    background-color: #3a3a3c; color: #f2f2f7;
    border: 1px solid #48484a; border-radius: 8px;
    padding: 4px 8px; font-size: 9.5pt;
    font-family: "SF Mono", "JetBrains Mono", monospace;
}
spinbutton:focus { border-color: #0a84ff; }
spinbutton button {
    background: transparent; border: none;
    border-left: 1px solid #48484a; border-radius: 0;
    padding: 2px 7px; color: #0a84ff;
}
spinbutton button:hover { background-color: #48484a; }

combobox button {
    background-color: #3a3a3c; color: #f2f2f7;
    border: 1px solid #48484a; border-radius: 8px;
    padding: 5px 10px; font-size: 9.5pt;
}
combobox button:hover { border-color: #0a84ff; }

entry {
    background-color: #3a3a3c; color: #f2f2f7;
    border: 1px solid #48484a; border-radius: 8px;
    padding: 5px 10px; font-size: 9.5pt;
    caret-color: #0a84ff;
}
entry:focus { border-color: #0a84ff; }

/* ── Scale ── */
scale trough {
    background-color: #3a3a3c; border: 1px solid #48484a;
    border-radius: 4px; min-height: 4px;
}
scale highlight { background-color: #0a84ff; border-radius: 4px; }
scale slider {
    background-color: #0a84ff; border-radius: 50%;
    min-width: 13px; min-height: 13px; border: 2px solid #5ac8fa;
}

/* ── Progress ── */
progressbar trough {
    background-color: #3a3a3c; border: none;
    border-radius: 6px; min-height: 6px;
}
progressbar progress {
    background: linear-gradient(90deg, #0066cc, #0a84ff);
    border-radius: 6px;
}
progressbar text {
    color: #636366; font-size: 7.5pt;
    font-family: "SF Mono", monospace;
}

/* ── Metrics ── */
.metric-key { color: #636366; font-size: 8.5pt; font-weight: 500; }
.metric-val {
    color: #ebebf5; font-size: 9pt; font-weight: 600;
    font-family: "SF Mono", "JetBrains Mono", monospace;
}
.metric-pos {
    color: #30d158; font-weight: 700; font-size: 9pt;
    font-family: "SF Mono", "JetBrains Mono", monospace;
}
.metric-neg {
    color: #ff453a; font-weight: 700; font-size: 9pt;
    font-family: "SF Mono", "JetBrains Mono", monospace;
}
.ks-ok   { color: #30d158; font-size: 8pt; font-weight: 700; font-family: monospace; padding: 3px 12px; }
.ks-warn { color: #ff453a; font-size: 8pt; font-weight: 700; font-family: monospace; padding: 3px 12px; }

/* ── Text / Log ── */
textview {
    background-color: #1c1c1e; color: #636366;
    font-family: "SF Mono", "JetBrains Mono", monospace;
    font-size: 8.5pt; padding: 8px;
}
textview text { background-color: #1c1c1e; }

/* ── TreeView ── */
treeview {
    background-color: #1c1c1e; color: #ebebf5;
    font-size: 8.5pt;
    font-family: "SF Mono", "JetBrains Mono", monospace;
}
treeview:selected { background-color: #0a3a6a; }
treeview header button {
    background-color: #2c2c2e; color: #aeaeb2;
    border: none; border-bottom: 1px solid #3a3a3c;
    font-size: 8pt; font-weight: 700; letter-spacing: 0.8px;
    text-transform: uppercase; padding: 6px 8px; border-radius: 0;
}
treeview header button:hover { background-color: #3a3a3c; }

/* ── Notebook ── */
notebook > header {
    background-color: #252525;
    border-bottom: 1px solid #2c2c2e;
    padding: 4px 8px 0 8px;
}
notebook > header tab {
    background-color: transparent; color: #636366;
    border-radius: 6px 6px 0 0; padding: 8px 18px;
    font-size: 9.5pt; font-weight: 500;
    border-bottom: 2px solid transparent; margin: 0 2px;
}
notebook > header tab:checked {
    color: #0a84ff; border-bottom: 2px solid #0a84ff;
    background-color: rgba(10,132,255,0.08); font-weight: 600;
}
notebook > header tab:hover:not(:checked) { color: #aeaeb2; }

/* ── Scrollbar ── */
scrollbar { background-color: transparent; }
scrollbar trough { background: transparent; min-width: 5px; min-height: 5px; }
scrollbar slider {
    background-color: #48484a; border-radius: 4px;
    min-width: 5px; min-height: 5px;
}
scrollbar slider:hover { background-color: #636366; }

/* ── Misc ── */
.theme-toggle {
    background: #3a3a3c; border: 1px solid #48484a;
    border-radius: 20px; padding: 4px 11px;
    font-size: 11pt; color: #aeaeb2; min-width: 0;
}
.theme-toggle:hover { background-color: #48484a; }
paned > separator { background-color: #2c2c2e; }
)css";

    static constexpr const char* LIGHT_CSS = R"css(
/* ── Base ─────────────────────────────────────────────────────────────────── */
window {
    background-color: #f4f5f8;
    color: #111827;
    font-family: -apple-system, "Inter", "SF Pro Text", "Segoe UI", system-ui, sans-serif;
    font-size: 10pt;
}
.view, scrolledwindow > viewport > * { background-color: #f4f5f8; color: #111827; }
popover, popover > contents, popover contents {
    background-color: #ffffff; color: #111827;
    border: 1px solid #e5e7eb; border-radius: 12px;
    box-shadow: 0 12px 32px rgba(17,24,39,0.14);
}

/* ── Sidebar ──────────────────────────────────────────────────────────────── */
.sidebar { background-color: #ffffff; border-right: 1px solid #e9ebef; }
.sidebar-title {
    font-size: 17pt; font-weight: 800; color: #0f1222;
    letter-spacing: -0.5px; padding: 16px 16px 0 16px;
}
.sidebar-subtitle {
    font-size: 7.5pt; color: #6366f1; font-weight: 700;
    letter-spacing: 1.8px; text-transform: uppercase; padding: 2px 16px 14px 16px;
}
.section-label {
    font-size: 7.5pt; font-weight: 700; letter-spacing: 1.6px;
    color: #9aa1ad; text-transform: uppercase; padding: 14px 16px 6px 16px;
}

/* ── Cards ────────────────────────────────────────────────────────────────── */
frame {
    border: 1px solid #ebedf1; border-radius: 14px;
    background-color: #ffffff; margin: 4px 10px;
    box-shadow: 0 1px 2px rgba(17,24,39,0.04), 0 6px 16px rgba(17,24,39,0.04);
}
frame > label {
    color: #8b90a0; font-size: 8pt; font-weight: 700;
    letter-spacing: 0.6px; text-transform: uppercase; padding: 2px 8px;
}
separator { background-color: #eceef2; min-height: 1px; margin: 6px 12px; }

/* ── Buttons ──────────────────────────────────────────────────────────────── */
button {
    background-color: #ffffff; color: #1f2430;
    border: 1px solid #e3e6ec; border-radius: 10px;
    padding: 7px 14px; font-size: 9.5pt; font-weight: 600;
}
button:hover  { background-color: #f7f8fa; border-color: #d3d7e0; }
button:active { background-color: #eef0f4; }
button:disabled { color: #b6bbc6; border-color: #eef0f4; }

.run-btn {
    background: linear-gradient(135deg, #6366f1 0%, #4f46e5 100%);
    color: #fff; border: none; border-radius: 11px;
    font-weight: 700; font-size: 10.5pt; padding: 9px 16px;
    box-shadow: 0 4px 14px rgba(79,70,229,0.32);
}
.run-btn:hover    { background: linear-gradient(135deg, #7c7ff5 0%, #6366f1 100%); }
.run-btn:disabled { background: #e7e9ef; color: #b6bbc6; box-shadow: none; }

.stop-btn {
    background: linear-gradient(135deg, #fb7185 0%, #ef4444 100%);
    color: #fff; border: none; border-radius: 11px;
    font-weight: 700; font-size: 10.5pt; padding: 9px 16px;
    box-shadow: 0 4px 14px rgba(239,68,68,0.28);
}
.stop-btn:hover    { background: linear-gradient(135deg, #fb8a98 0%, #fb7185 100%); }
.stop-btn:disabled { background: #fdeef0; color: #f4b8bf; box-shadow: none; }

/* ── Inputs ───────────────────────────────────────────────────────────────── */
spinbutton, entry {
    background-color: #ffffff; color: #111827;
    border: 1px solid #e3e6ec; border-radius: 10px;
    padding: 6px 10px; font-size: 9.5pt;
    font-family: "SF Mono", "JetBrains Mono", ui-monospace, monospace;
}
spinbutton:focus, entry:focus, combobox button:focus {
    border-color: #6366f1; box-shadow: 0 0 0 3px rgba(99,102,241,0.18);
}
spinbutton button { background: transparent; border: none; border-left: 1px solid #eceef2; border-radius: 0; padding: 2px 8px; color: #6366f1; }
spinbutton button:hover { background-color: #f3f4f8; }
combobox button { background-color: #ffffff; color: #111827; border: 1px solid #e3e6ec; border-radius: 10px; padding: 6px 11px; font-size: 9.5pt; font-weight: 500; }
combobox button:hover { border-color: #c9cdd8; }
entry { caret-color: #6366f1; }

/* ── Scale ────────────────────────────────────────────────────────────────── */
scale trough { background-color: #eceef2; border: none; border-radius: 6px; min-height: 5px; }
scale highlight { background-color: #6366f1; border-radius: 6px; }
scale slider { background-color: #ffffff; border-radius: 50%; min-width: 16px; min-height: 16px; border: 2px solid #6366f1; box-shadow: 0 1px 4px rgba(17,24,39,0.18); }

/* ── Progress ─────────────────────────────────────────────────────────────── */
progressbar trough { background-color: #eceef2; border: none; border-radius: 8px; min-height: 8px; }
progressbar progress { background: linear-gradient(90deg, #6366f1, #818cf8); border-radius: 8px; }
progressbar text { color: #8b90a0; font-size: 7.5pt; font-family: "SF Mono", monospace; }

/* ── Metrics ──────────────────────────────────────────────────────────────── */
.metric-key { color: #8b90a0; font-size: 8.5pt; font-weight: 500; }
.metric-val { color: #1f2430; font-size: 10pt; font-weight: 700; font-family: "SF Mono", "JetBrains Mono", monospace; }
.metric-pos { color: #059669; font-weight: 800; font-family: "SF Mono", "JetBrains Mono", monospace; font-size: 10pt; }
.metric-neg { color: #e11d48; font-weight: 800; font-family: "SF Mono", "JetBrains Mono", monospace; font-size: 10pt; }
.ks-ok   { color: #059669; font-size: 8.5pt; font-weight: 800; font-family: monospace; padding: 4px 14px; }
.ks-warn { color: #e11d48; font-size: 8.5pt; font-weight: 800; font-family: monospace; padding: 4px 14px; }

/* ── Text / Log ───────────────────────────────────────────────────────────── */
textview { background-color: #ffffff; color: #5b616e; font-family: "SF Mono", "JetBrains Mono", monospace; font-size: 8.5pt; padding: 10px; }
textview text { background-color: #ffffff; }

/* ── TreeView (fill blotter) ──────────────────────────────────────────────── */
treeview { background-color: #ffffff; color: #1f2430; font-size: 9pt; font-family: "SF Mono", "JetBrains Mono", monospace; }
treeview:selected { background-color: #eef0ff; color: #4338ca; }
treeview header button {
    background-color: #fbfbfd; color: #9aa1ad; border: none;
    border-bottom: 1px solid #eceef2; font-size: 7.5pt; font-weight: 700;
    letter-spacing: 1px; text-transform: uppercase; padding: 8px 10px; border-radius: 0;
}
treeview header button:hover { background-color: #f3f4f8; }

/* ── Notebook ─────────────────────────────────────────────────────────────── */
notebook > header { background-color: #f4f5f8; border-bottom: 1px solid #e9ebef; padding: 6px 10px 0 10px; }
notebook > header tab {
    background-color: transparent; color: #8b90a0;
    border-radius: 9px 9px 0 0; padding: 9px 20px;
    font-size: 9.5pt; font-weight: 600; border-bottom: 2px solid transparent; margin: 0 2px;
}
notebook > header tab:checked { color: #4f46e5; border-bottom: 2px solid #4f46e5; background-color: rgba(99,102,241,0.07); }
notebook > header tab:hover:not(:checked) { color: #4b5563; }

/* ── Scrollbar ────────────────────────────────────────────────────────────── */
scrollbar { background-color: transparent; }
scrollbar trough { background-color: transparent; border-radius: 6px; min-width: 6px; min-height: 6px; }
scrollbar slider { background-color: #d3d7e0; border-radius: 6px; min-width: 6px; min-height: 6px; }
scrollbar slider:hover { background-color: #b2b8c4; }

/* ── Theme toggle ─────────────────────────────────────────────────────────── */
.theme-toggle { background: #f3f4f8; border: 1px solid #e3e6ec; border-radius: 20px; padding: 5px 13px; font-size: 10pt; font-weight: 600; color: #4f46e5; min-width: 0; }
.theme-toggle:hover { background-color: #eceef4; }

paned > separator { background-color: #e9ebef; }

/* ── Embedded terminal (dark output, light chrome) ────────────────────────── */
.quick-btn { background-color: #f3f4f8; border: 1px solid #e3e6ec; border-radius: 9px; padding: 5px 12px; font-size: 8.5pt; font-weight: 600; color: #4f46e5; }
.quick-btn:hover { background-color: #eceef4; border-color: #c9cdd8; }
.console-prompt { color: #6366f1; font-size: 13pt; font-weight: 800; font-family: "SF Mono", monospace; padding: 0 4px; }
.console-input { font-family: "SF Mono", "JetBrains Mono", monospace; }
textview.console-out, textview.console-out text {
    background-color: #0d1117; color: #d7dde6;
    font-family: "SF Mono", "JetBrains Mono", monospace; font-size: 9pt; padding: 12px;
}
)css";

    void apply_css() {
        css_provider_ = Gtk::CssProvider::create();
        css_provider_->load_from_data(dark_mode_ ? DARK_CSS : LIGHT_CSS);
        Gtk::StyleContext::add_provider_for_display(
            get_display(), css_provider_,
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    void toggle_theme() {
        dark_mode_ = !dark_mode_;
        theme_btn_.set_label(dark_mode_ ? "Light" : "Dark");
        // Remove old provider, apply new
        Gtk::StyleContext::remove_provider_for_display(get_display(), css_provider_);
        apply_css();
        // Update Cairo backgrounds
        equity_curve_.set_dark(dark_mode_);
        depth_chart_.set_dark(dark_mode_);
        editor_panel_.set_dark(dark_mode_);
        equity_curve_.queue_draw();
        depth_chart_.queue_draw();
    }

    // ── Button handlers ───────────────────────────────────────────────────────

    void on_run() {
        std::string strat = strategy_box_.get_active_id();
        int    ticks = (int)ticks_spin_.get_value();
        double sigma = sigma_spin_.get_value();
        int    seed  = (int)seed_spin_.get_value();
        double cash  = cash_spin_.get_value();

        std::string source = source_box_.get_active_id();
        std::string symbol = symbol_entry_.get_text().empty() ? "AAPL"
                              : std::string(symbol_entry_.get_text());
        std::string period   = period_box_.get_active_id();
        std::string interval = interval_box_.get_active_id();

        log("Starting " + strat + " | source=" + source
            + (source != "synthetic" ? " symbol=" + symbol : "")
            + " ticks=" + std::to_string(ticks));

        run_btn_.set_sensitive(false);
        stop_btn_.set_sensitive(true);
        fill_store_->clear();
        fill_row_count_ = 0;

        if (source != "synthetic") {
            run_external_python(strat, source, symbol, period, interval, ticks, seed);
            return;
        }

        runner_.start(strat, ticks, sigma, seed, cash, [this]{
            dispatcher_.emit();
        });

        // Periodic refresh during sim
        Glib::signal_timeout().connect([this]() -> bool {
            if (state_.done) { dispatcher_.emit(); return false; }
            dispatcher_.emit();
            return true;
        }, 100);   // refresh every 100ms
    }

    void run_external_python(const std::string& strat,
                             const std::string& source,
                             const std::string& symbol,
                             const std::string& period,
                             const std::string& interval,
                             int /*ticks*/, int seed) {
        std::string cmd;
        if (source == "crypto") {
            // Live crypto: stream from quantsim.live.run. Background the python
            // and record its PID so Stop can SIGTERM it (clean shutdown).
            std::string lstrat = strat;
            if (lstrat == "stat_arb") lstrat = "meanrev";   // map equity-only id
            double cash = cash_spin_.get_value();
            std::ostringstream c;
            c << "PYTHONPATH=python python3 -u -m quantsim.live.run"
              << " --symbol "   << symbol
              << " --strategy " << lstrat
              << " --cash "     << std::fixed << std::setprecision(0) << cash
              << " --duration 0"
              << " 2>>hft_gui_run.log"
              << " & echo $! > .quantsim_live.pid; wait";
            cmd = c.str();
            state_.initial_cash = cash;
            state_.live_mode = true;
            log("[LIVE] Binance " + symbol + " → " + lstrat + "  (paper trading)");
        } else {
            std::string script;
            if      (strat == "mm")       script = "examples/market_maker.py";
            else if (strat == "stat_arb") script = "examples/stat_arb.py";
            else                          script = "examples/twap.py";

            std::string args = " --source "   + source
                             + " --symbol "   + symbol
                             + " --period "   + period
                             + " --interval " + interval
                             + " --seed "     + std::to_string(seed);
            if (strat == "twap")     args += " --qty 5000";
            if (strat == "stat_arb") args += " --sym_a " + symbol;

            // QUANTSIM_STREAM=1 makes BacktestEngine print "EQ:{value}" each tick.
            cmd = "QUANTSIM_STREAM=1 python3 " + script + args + " 2>/dev/null";
            state_.initial_cash = 500'000.0;
            state_.live_mode = false;
            log("[DATA] " + source + " → " + symbol + " (" + period + "/" + interval + ")");
        }

        // Reset state for fresh run
        state_.equity.clear();
        state_.fill_log.clear();
        state_.live_bids.clear(); state_.live_asks.clear();
        state_.fills_total = state_.orders_sent = state_.orders_blocked = 0;
        state_.pos_net = state_.pos_avg = state_.pos_realized = state_.pos_unreal = 0;
        state_.risk_state = "OK"; state_.risk_detail.clear();
        state_.running = true;
        state_.done    = false;
        state_.tick_num = 0;

        ext_running_.store(true);
        ext_done_ = false;

        std::thread([this, cmd]{
            FILE* p = popen(cmd.c_str(), "r");
            if (!p) {
                std::lock_guard lk(ext_mu_);
                ext_output_ = "[ERROR] Failed to launch python3\n";
                ext_done_ = true; ext_running_.store(false);
                dispatcher_.emit();
                return;
            }
            char buf[1024];
            while (fgets(buf, sizeof(buf), p)) {
                std::lock_guard lk(ext_mu_);
                ext_output_ += buf;
            }
            pclose(p);
            ext_done_ = true;
            ext_running_.store(false);
            dispatcher_.emit();
        }).detach();

        Glib::signal_timeout().connect([this]() -> bool {
            std::string raw;
            {
                std::lock_guard lk(ext_mu_);
                raw.swap(ext_output_);
            }

            if (!raw.empty()) {
                // Parse structured lines; rest → log
                std::istringstream ss(raw);
                std::string line;
                std::string log_chunk;
                while (std::getline(ss, line)) {
                    if (line.size() > 3 && line[0]=='E' && line[1]=='Q' && line[2]==':') {
                        // EQ:{equity}
                        try {
                            state_.push_equity(std::stod(line.substr(3)));
                            state_.tick_num++;
                        } catch (...) {}

                    } else if (line.rfind("BA:",0)==0) {
                        // BA:{bid},{ask}
                        auto comma = line.find(',', 3);
                        if (comma != std::string::npos) {
                            try {
                                double bid = std::stod(line.substr(3, comma-3));
                                double ask = std::stod(line.substr(comma+1));
                                if (!state_.live_mode) {
                                    std::vector<DepthLevel> bids = {{bid, 100, 1}};
                                    std::vector<DepthLevel> asks = {{ask, 100, 1}};
                                    state_.set_book(bid, ask, 100, 100,
                                                    std::move(bids), std::move(asks));
                                } else {
                                    std::lock_guard lk(state_.mu);
                                    state_.best_bid = bid; state_.best_ask = ask;
                                }
                            } catch (...) {}
                        }

                    } else if (line.rfind("DEPTH:",0)==0) {
                        // DEPTH:{p:q|p:q|...};{p:q|...}  — full L2 ladder
                        auto semi = line.find(';', 6);
                        if (semi != std::string::npos) {
                            auto parse_side = [](const std::string& s){
                                std::vector<std::pair<double,double>> out;
                                size_t i = 0;
                                while (i < s.size()) {
                                    size_t bar = s.find('|', i);
                                    std::string tok = (bar==std::string::npos)
                                        ? s.substr(i) : s.substr(i, bar-i);
                                    auto colon = tok.find(':');
                                    if (colon != std::string::npos) {
                                        try {
                                            out.emplace_back(std::stod(tok.substr(0,colon)),
                                                             std::stod(tok.substr(colon+1)));
                                        } catch (...) {}
                                    }
                                    if (bar==std::string::npos) break;
                                    i = bar + 1;
                                }
                                return out;
                            };
                            auto bidss = parse_side(line.substr(6, semi-6));
                            auto askss = parse_side(line.substr(semi+1));
                            double bb = bidss.empty()?0:bidss.front().first;
                            double ba = askss.empty()?0:askss.front().first;
                            state_.set_live_book(bb, ba, std::move(bidss), std::move(askss));
                        }

                    } else if (line.rfind("FILL:",0)==0) {
                        // FILL:{side}:{qty}:{price}:{symbol}[:{maker}]
                        std::vector<std::string> tok;
                        std::istringstream ps(line.substr(5)); std::string t;
                        while (std::getline(ps, t, ':')) tok.push_back(t);
                        if (tok.size() >= 4) {
                            SimState::FillRow row;
                            row.id    = ++fill_disp_id_;
                            row.side  = tok[0];
                            try { row.qty   = std::stod(tok[1]); } catch (...) { row.qty=0; }
                            try { row.price = std::stod(tok[2]); } catch (...) { row.price=0; }
                            row.sym   = tok[3];
                            row.maker = (tok.size() >= 5 && tok[4] == "1");
                            row.ts_ns = 0;
                            state_.push_fill(std::move(row));     // bumps fills_total
                            if (!state_.live_mode) ++state_.orders_sent;
                        }

                    } else if (line.rfind("POS:",0)==0) {
                        // POS:{net}:{avg}:{realized}:{unreal}
                        std::vector<std::string> tok;
                        std::istringstream ps(line.substr(4)); std::string t;
                        while (std::getline(ps, t, ':')) tok.push_back(t);
                        if (tok.size() >= 4) {
                            try {
                                state_.pos_net      = std::stod(tok[0]);
                                state_.pos_avg      = std::stod(tok[1]);
                                state_.pos_realized = std::stod(tok[2]);
                                state_.pos_unreal   = std::stod(tok[3]);
                            } catch (...) {}
                        }

                    } else if (line.rfind("STAT:",0)==0) {
                        std::istringstream ss2(line.substr(5)); std::string kv;
                        while (std::getline(ss2, kv, ';')) {
                            auto eq = kv.find('=');
                            if (eq==std::string::npos) continue;
                            std::string k=kv.substr(0,eq), v=kv.substr(eq+1);
                            try {
                                if      (k=="sharpe")      state_.sharpe = std::stod(v);
                                else if (k=="sortino")     state_.sortino= std::stod(v);
                                else if (k=="max_dd")      state_.max_dd = std::stod(v);
                                else if (k=="vol")         state_.vol    = std::stod(v);
                                else if (k=="fills")       state_.fills_total   = std::stoull(v);
                                else if (k=="orders")      state_.orders_sent   = std::stoull(v);
                                else if (k=="blocked")     state_.orders_blocked= std::stoull(v);
                                else if (k=="maker_ratio") state_.maker_ratio   = std::stod(v);
                            } catch (...) {}
                        }

                    } else if (line.rfind("RISK:",0)==0) {
                        auto colon = line.find(':',5);
                        state_.risk_state  = line.substr(5, colon==std::string::npos?
                                                            std::string::npos : colon-5);
                        state_.risk_detail = (colon==std::string::npos)? "" : line.substr(colon+1);
                        state_.kill_armed  = (state_.risk_state=="ARMED" ||
                                              state_.risk_state=="BLOCKED");

                    } else if (line.rfind("LAT:",0)==0) {
                        try { state_.feed_lat_us = std::stod(line.substr(4)); } catch (...) {}

                    } else if (line.rfind("MID:",0)==0) {
                        // informational; mid already implied by BA/DEPTH

                    } else if (!line.empty()) {
                        if (line.rfind("LOG:",0)==0) log_chunk += line.substr(4) + "\n";
                        else                          log_chunk += line + "\n";
                    }
                }
                if (!log_chunk.empty()) {
                    log_buf_->insert(log_buf_->end(), log_chunk);
                    auto mark = log_buf_->create_mark(log_buf_->end(), false);
                    log_view_.scroll_to(mark);
                    results_buf_->insert(results_buf_->end(), log_chunk);
                }

                // Refresh charts
                {
                    std::lock_guard lk(state_.mu);
                    equity_curve_.set_data(state_.equity, state_.initial_cash);
                    if (state_.live_mode)
                        depth_chart_.update_levels(state_.live_bids, state_.live_asks,
                                                   state_.best_bid, state_.best_ask);
                    else
                        depth_chart_.update(state_.bids, state_.asks,
                                            state_.best_bid, state_.best_ask);
                }
                equity_curve_.queue_draw();
                depth_chart_.queue_draw();

                // Append new fills to fill table
                {
                    std::lock_guard lk(state_.mu);
                    int new_rows = (int)state_.fill_log.size() - fill_row_count_;
                    if (new_rows > 0) {
                        auto it = state_.fill_log.begin();
                        std::advance(it, fill_row_count_);
                        for (; it != state_.fill_log.end(); ++it) {
                            auto row = fill_store_->prepend();
                            std::ostringstream qs;
                            if (it->qty >= 1) qs << std::fixed << std::setprecision(3) << it->qty;
                            else              qs << std::fixed << std::setprecision(5) << it->qty;
                            std::string side = it->side;
                            if (state_.live_mode) side += it->maker ? " ·M" : " ·T";
                            (*row)[fill_cols_.col_id]    = std::to_string(it->id);
                            (*row)[fill_cols_.col_sym]   = it->sym;
                            (*row)[fill_cols_.col_side]  = side;
                            (*row)[fill_cols_.col_qty]   = qs.str();
                            std::ostringstream px; px << std::fixed << std::setprecision(2) << it->price;
                            (*row)[fill_cols_.col_price] = px.str();
                        }
                        fill_row_count_ = (int)state_.fill_log.size();
                    }
                }
                lbl_fills_.set_text(std::to_string(state_.fills_total));
                lbl_orders_.set_text(std::to_string((uint64_t)state_.orders_sent));
                comparison_panel_.update(state_);  // live comparison refresh

                // Live: surface streamed metrics, position, risk, latency
                if (state_.live_mode) {
                    auto f3 = [](double v,int dp=3){ std::ostringstream s;
                        s<<std::fixed<<std::setprecision(dp)<<v; return s.str(); };
                    lbl_sharpe_.set_text(f3(state_.sharpe));
                    lbl_sortino_.set_text(f3(state_.sortino));
                    lbl_dd_.set_text(f3(state_.max_dd*100,2)+"%");
                    lbl_vol_.set_text(f3(state_.vol*100,3)+"%");
                    std::ostringstream pos; pos<<std::showpos<<std::fixed
                        <<std::setprecision(5)<<state_.pos_net;
                    lbl_pos_.set_text(pos.str());
                    lbl_lat_.set_text(f3(state_.feed_lat_us,0)+" µs");
                    // risk badge
                    kill_label_.get_style_context()->remove_class("ks-ok");
                    kill_label_.get_style_context()->remove_class("ks-warn");
                    if (state_.kill_armed) {
                        kill_label_.get_style_context()->add_class("ks-warn");
                        kill_label_.set_text("● RISK: " + state_.risk_state +
                                             (state_.risk_detail.empty()? "" : " — " + state_.risk_detail));
                    } else {
                        kill_label_.get_style_context()->add_class("ks-ok");
                        kill_label_.set_text("● RISK: OK");
                    }
                }

                // Update progress
                if (!state_.equity.empty()) {
                    double pnl = state_.equity.back() - state_.initial_cash;
                    std::ostringstream ps;
                    ps << (pnl>=0?"+":"") << "$" << std::fixed
                       << std::setprecision(0) << pnl
                       << "  (" << state_.tick_num << " ticks)";
                    progress_bar_.set_text(ps.str());
                    progress_bar_.set_fraction(ext_done_ ? 1.0 : 0.5);
                }

                // Live PnL label
                if (!state_.equity.empty()) {
                    double pnl = state_.equity.back() - state_.initial_cash;
                    std::ostringstream pnl_ss;
                    pnl_ss << (pnl>=0?"+":"") << "$"
                           << std::fixed << std::setprecision(2) << pnl;
                    lbl_pnl_.get_style_context()->remove_class("metric-pos");
                    lbl_pnl_.get_style_context()->remove_class("metric-neg");
                    lbl_pnl_.get_style_context()->add_class(pnl>=0?"metric-pos":"metric-neg");
                    lbl_pnl_.set_text(pnl_ss.str());
                    lbl_ticks_.set_text(std::to_string(state_.tick_num));
                }
            }

            if (ext_done_) {
                state_.compute_metrics();
                state_.save_to_prev();  // save current → previous for comparison
                state_.running = false;
                state_.done    = true;
                run_btn_.set_sensitive(true);
                stop_btn_.set_sensitive(false);
                progress_bar_.set_fraction(1.0);
                progress_bar_.set_text("Complete");
                // Final metrics
                auto fmt = [](double v, int dp=3){
                    std::ostringstream s;
                    s << std::fixed << std::setprecision(dp) << v;
                    return s.str();
                };
                lbl_sharpe_.set_text(fmt(state_.sharpe));
                lbl_sortino_.set_text(fmt(state_.sortino));
                lbl_dd_.set_text(fmt(state_.max_dd*100,2)+"%");
                lbl_vol_.set_text(fmt(state_.vol*100,3)+"%");
                lbl_fills_.set_text(std::to_string(state_.fills_total));
                comparison_panel_.update(state_);
                // Final equity curve redraw
                {
                    std::lock_guard lk(state_.mu);
                    equity_curve_.set_data(state_.equity, state_.initial_cash);
                }
                equity_curve_.queue_draw();
                return false;
            }
            return true;
        }, 100);
    }

    void on_stop() {
        log("Stop requested");
        runner_.stop();
        if (state_.live_mode) stop_live_process();
        run_btn_.set_sensitive(true);
        stop_btn_.set_sensitive(false);
        progress_bar_.set_text("Stopped");
    }

    // Send SIGTERM to the live python subprocess; it finalizes and exits cleanly.
    void stop_live_process() {
        std::ifstream pf(".quantsim_live.pid");
        if (pf) {
            long pid = 0; pf >> pid;
            if (pid > 0) {
                ::kill((pid_t)pid, SIGTERM);
                log("[LIVE] sent SIGTERM to pid " + std::to_string(pid));
            }
        }
        std::remove(".quantsim_live.pid");
    }

    // ── UI Refresh (called from main thread via dispatcher) ───────────────────

    void refresh_ui() {
        uint64_t tk  = state_.tick_num;
        uint64_t ttl = state_.total_ticks;

        // Progress
        double frac = ttl > 0 ? (double)tk/ttl : 0.0;
        progress_bar_.set_fraction(frac);
        progress_bar_.set_text(std::to_string(tk) + " / " + std::to_string(ttl));

        // Metrics labels
        auto fmt = [](double v, int dp=3) {
            std::ostringstream ss; ss << std::fixed << std::setprecision(dp) << v;
            return ss.str();
        };

        auto set_metric = [](Gtk::Label& lbl, const std::string& val,
                              bool positive = true, bool colored = false) {
            if (colored) {
                lbl.get_style_context()->remove_class("metric-pos");
                lbl.get_style_context()->remove_class("metric-neg");
                lbl.get_style_context()->add_class(positive ? "metric-pos" : "metric-neg");
            } else {
                lbl.get_style_context()->remove_class("metric-pos");
                lbl.get_style_context()->remove_class("metric-neg");
                lbl.get_style_context()->add_class("metric-val");
            }
            lbl.set_text(val);
        };

        set_metric(lbl_sharpe_,  fmt(state_.sharpe),  state_.sharpe >= 0,  true);
        set_metric(lbl_sortino_, fmt(state_.sortino), state_.sortino >= 0, true);
        set_metric(lbl_dd_,      fmt(state_.max_dd*100,2)+"%", false, false);
        set_metric(lbl_vol_,     fmt(state_.vol*100,3)+"%",    false, false);
        set_metric(lbl_fills_,   std::to_string(state_.fills_total));
        set_metric(lbl_orders_,  std::to_string((uint64_t)state_.orders_sent));
        set_metric(lbl_ticks_,   std::to_string(tk));

        double pnl = state_.equity.empty() ? 0.0 : state_.equity.back()-state_.initial_cash;
        std::ostringstream pnl_ss;
        pnl_ss << (pnl>=0?"+":"") << "$" << std::fixed << std::setprecision(2) << pnl;
        set_metric(lbl_pnl_, pnl_ss.str(), pnl >= 0, true);

        // Kill switch — use CSS classes, not markup
        if (state_.kill_armed) {
            kill_label_.get_style_context()->remove_class("ks-ok");
            kill_label_.get_style_context()->add_class("ks-warn");
            kill_label_.set_text("● KILL SWITCH: ARMED");
        } else {
            kill_label_.get_style_context()->remove_class("ks-warn");
            kill_label_.get_style_context()->add_class("ks-ok");
            kill_label_.set_text("● KILL SWITCH: OK");
        }

        // Equity curve
        {
            std::lock_guard lk(state_.mu);
            equity_curve_.set_data(state_.equity, state_.initial_cash);
        }

        // Depth chart
        {
            std::lock_guard lk(state_.mu);
            depth_chart_.update(state_.bids, state_.asks, state_.best_bid, state_.best_ask);
        }

        // Fill log — append new rows
        {
            std::lock_guard lk(state_.mu);
            int new_rows = (int)state_.fill_log.size() - fill_row_count_;
            if (new_rows > 0) {
                auto start = state_.fill_log.begin();
                std::advance(start, fill_row_count_);
                for (auto it=start; it!=state_.fill_log.end(); ++it) {
                    auto row = fill_store_->prepend();
                    std::ostringstream id_ss; id_ss << it->id;
                    std::ostringstream qty_ss; qty_ss << it->qty;
                    std::ostringstream px_ss; px_ss << std::fixed << std::setprecision(4) << it->price;
                    (*row)[fill_cols_.col_id]   = id_ss.str();
                    (*row)[fill_cols_.col_sym]  = it->sym;
                    (*row)[fill_cols_.col_side] = it->side;
                    (*row)[fill_cols_.col_qty]  = qty_ss.str();
                    (*row)[fill_cols_.col_price]= px_ss.str();
                }
                fill_row_count_ = (int)state_.fill_log.size();
            }
        }

        // On done
        if (state_.done) {
            run_btn_.set_sensitive(true);
            stop_btn_.set_sensitive(false);
            progress_bar_.set_fraction(1.0);
            progress_bar_.set_text("Complete");
            show_results();
            log("Simulation complete. PnL = $" + fmt(pnl,2)
                + "  Sharpe = " + fmt(state_.sharpe));
        }
    }

    void show_results() {
        std::ostringstream ss;
        ss << "═══════════════════════════════════════════════════════\n";
        ss << "  QUANTSIM BACKTEST RESULTS\n";
        ss << "═══════════════════════════════════════════════════════\n\n";
        ss << "  Strategy:       " << state_.strategy_name << "\n";
        ss << "  Ticks:          " << state_.tick_num << " / " << state_.total_ticks << "\n";
        ss << "  Initial Cash:   $" << std::fixed << std::setprecision(2) << state_.initial_cash << "\n";
        ss << "  Final Equity:   $";
        if (!state_.equity.empty())
            ss << std::fixed << std::setprecision(2) << state_.equity.back() << "\n";
        else ss << "N/A\n";
        ss << "  PnL:            $" << std::fixed << std::setprecision(2) << state_.final_pnl;
        ss << "  (" << std::setprecision(3) << state_.final_pnl/state_.initial_cash*100 << "%)\n\n";
        ss << "  ── Risk Metrics ──────────────────────────────────────\n";
        ss << "  Sharpe Ratio:   " << std::fixed << std::setprecision(4) << state_.sharpe << "\n";
        ss << "  Sortino Ratio:  " << state_.sortino << "\n";
        ss << "  Calmar Ratio:   " << state_.calmar  << "\n";
        ss << "  Max Drawdown:   " << std::setprecision(3) << state_.max_dd*100 << "%\n";
        ss << "  Volatility:     " << state_.vol*100 << "% (annualized)\n\n";
        ss << "  ── Execution Metrics ─────────────────────────────────\n";
        ss << "  Total Fills:    " << state_.fills_total << "\n";
        ss << "  Orders Sent:    " << (uint64_t)state_.orders_sent << "\n";
        ss << "  Fill Rate:      ";
        if (state_.orders_sent > 0)
            ss << std::setprecision(1) << 100.0*state_.fills_total/state_.orders_sent << "%\n";
        else ss << "N/A\n";
        ss << "\n═══════════════════════════════════════════════════════\n";
        results_buf_->set_text(ss.str());
    }

    void log(const std::string& msg) {
        auto end = log_buf_->end();
        log_buf_->insert(end, msg + "\n");
        // Auto-scroll
        auto mark = log_buf_->create_mark(log_buf_->end(), false);
        log_view_.scroll_to(mark);
        log_msg(msg);
    }

    // ── State & Widgets ───────────────────────────────────────────────────────

    SimState    state_;
    SimRunner   runner_;
    Glib::Dispatcher dispatcher_;

    // Theme
    bool dark_mode_{false};
    Glib::RefPtr<Gtk::CssProvider> css_provider_;
    Gtk::Button theme_btn_;

    // Controls
    Gtk::ComboBoxText strategy_box_;
    // Data source controls
    Gtk::ComboBoxText  source_box_;
    Gtk::Entry         symbol_entry_;
    Gtk::ComboBoxText  period_box_;
    Gtk::ComboBoxText  interval_box_;
    Gtk::Box*          real_data_box_{nullptr};
    Gtk::Label*        ds_period_lbl_{nullptr};
    Gtk::Label*        ds_interval_lbl_{nullptr};
    // External Python process state
    std::atomic<bool>  ext_running_{false};
    std::mutex         ext_mu_;
    std::string        ext_output_;
    bool               ext_done_{false};
    Gtk::SpinButton   ticks_spin_, sigma_spin_, seed_spin_, cash_spin_;
    Gtk::Scale        speed_scale_{Gtk::Orientation::HORIZONTAL};
    Gtk::Button       run_btn_, stop_btn_;
    Gtk::ProgressBar  progress_bar_;
    Gtk::Label        kill_label_;

    // Metric labels
    Gtk::Label lbl_sharpe_, lbl_sortino_, lbl_dd_, lbl_vol_;
    Gtk::Label lbl_fills_, lbl_orders_, lbl_pnl_, lbl_ticks_;
    Gtk::Label lbl_pos_, lbl_lat_;
    uint64_t   fill_disp_id_{0};

    // Visualizations
    EquityCurve  equity_curve_;
    DepthChart   depth_chart_;

    // Fill table
    struct FillCols : public Gtk::TreeModel::ColumnRecord {
        FillCols() { add(col_id); add(col_sym); add(col_side);
                     add(col_qty); add(col_price); }
        Gtk::TreeModelColumn<Glib::ustring> col_id, col_sym, col_side, col_qty, col_price;
    } fill_cols_;
    Glib::RefPtr<Gtk::ListStore> fill_store_;
    Gtk::TreeView*    fill_tree_view_{nullptr};
    Gtk::ScrolledWindow fill_scroll_;
    int fill_row_count_{0};

    // Log
    Gtk::ScrolledWindow log_scroll_;
    Gtk::TextView       log_view_;
    Glib::RefPtr<Gtk::TextBuffer> log_buf_;

    // Results
    Gtk::ScrolledWindow results_scroll_;
    Gtk::TextView       results_view_;
    Glib::RefPtr<Gtk::TextBuffer> results_buf_;

    // Editor tab
    EditorPanel editor_panel_;

    // Terminal/console tab
    ConsolePanel console_panel_;

    // Comparison tab
    ComparisonPanel comparison_panel_;
};

// ── Entry Point ───────────────────────────────────────────────────────────────

int run_hft_gui(int argc, char** argv) {
    log_msg("QuantSim GUI starting");
    try {
        auto app = Gtk::Application::create("org.quantsim.app");
        return app->make_window_and_run<QuantSimWindow>(argc, argv);
    } catch (const std::exception& e) {
        log_msg(std::string("Fatal: ") + e.what());
        return -1;
    }
}

} // namespace hft
