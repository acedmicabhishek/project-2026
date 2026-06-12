
#include "hft_simulator/ws_client.hpp"
#include "hft_simulator/strategies/custom_strat_api.hpp"
#include "json/json.hpp"

#include <dlfcn.h>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using json = nlohmann::json;
using hft::WsClient;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

static void emit(const std::string& s) { fputs(s.c_str(), stdout); fputc('\n', stdout); fflush(stdout); }
static std::string f4(double v) { char b[40]; snprintf(b, sizeof b, "%.4f", v); return b; }
static std::string f6(double v) { char b[40]; snprintf(b, sizeof b, "%.6f", v); return b; }

struct Book {
    std::vector<std::pair<double,double>> bids, asks;
    double best_bid() const { return bids.empty() ? 0.0 : bids.front().first; }
    double best_ask() const { return asks.empty() ? 0.0 : asks.front().first; }
    double mid()      const { double b=best_bid(), a=best_ask(); return (b>0&&a>0)?(b+a)/2:0.0; }
    double qty_at(double px) const {
        for (auto& l : bids) if (std::abs(l.first-px)<1e-9) return l.second;
        for (auto& l : asks) if (std::abs(l.first-px)<1e-9) return l.second;
        return 0.0;
    }
};

struct Position {
    double net=0, avg=0, realized=0, fees=0;
    void apply(char side, double qty, double px, double fee_bps) {
        double s = (side=='B') ? qty : -qty;
        double fee = std::abs(qty)*px*fee_bps/1e4;
        fees += fee; realized -= fee;
        if (net!=0 && ((net>0)!=(s>0))) {
            double closed = std::min(std::abs(net), std::abs(s));
            realized += closed*(px-avg)*(net>0?1:-1);
            double newnet = net + s;
            if ((net>0)==(newnet>0) || newnet==0) { net=newnet; if(net==0) avg=0; return; }
            net = newnet; avg = px;
            return;
        }
        double newnet = net + s;
        if (newnet!=0) avg = (avg*std::abs(net) + px*std::abs(s)) / std::abs(newnet);
        net = newnet;
    }
    double unreal(double mid) const { return net!=0 ? net*(mid-avg) : 0.0; }
};

struct Gateway {
    struct Resting { uint64_t id; char side; double px, qty, queue_ahead; };
    std::vector<Resting> resting;
    Position pos;
    double maker_fee_bps=0.0, taker_fee_bps=5.0;
    uint64_t next_id=1, fills=0, maker_fills=0, orders=0, blocked=0;
    std::string sym;

    double max_position=1e18, max_notional=1e18, max_loss=1e18;
    bool   halted=false;

    void log_fill(char side, double qty, double px, bool maker) {
        emit("FILL:" + std::string(side=='B'?"BUY":"SELL") + ":" + f6(qty) + ":" +
             f4(px) + ":" + sym + ":" + (maker?"1":"0"));
        ++fills; if (maker) ++maker_fills;
        emit("POS:" + f6(pos.net) + ":" + f4(pos.avg) + ":" +
             f4(pos.realized) + ":" + f4(pos.unreal(px)));
    }

    bool risk_ok(char side, double qty, double px) {
        double newpos = pos.net + (side=='B'?qty:-qty);
        if (std::abs(newpos) > max_position) { ++blocked;
            emit("RISK:BLOCKED:position limit"); return false; }
        if (std::abs(newpos)*px > max_notional) { ++blocked;
            emit("RISK:BLOCKED:notional limit"); return false; }
        return true;
    }

    void cancel_all() { resting.clear(); }

    void post(char side, double qty, double px, const Book& bk, bool post_only) {
        if (halted || qty<=0) return;
        if (post_only) {
            if (side=='B' && bk.best_ask()>0 && px>=bk.best_ask()) return;
            if (side=='S' && bk.best_bid()>0 && px<=bk.best_bid()) return;
        }
        if (!risk_ok(side, qty, px)) return;
        ++orders;
        resting.push_back({next_id++, side, px, qty, bk.qty_at(px)});
    }

    void taker(char side, double qty, const Book& bk) {
        if (halted || qty<=0) return;
        if (!risk_ok(side, qty, bk.mid()>0?bk.mid():(bk.best_ask()+bk.best_bid())/2)) return;
        ++orders;
        double rem = qty;
        const auto& levels = (side=='B') ? bk.asks : bk.bids;
        for (auto& l : levels) {
            if (rem<=0) break;
            double f = std::min(rem, l.second);
            pos.apply(side, f, l.first, taker_fee_bps);
            log_fill(side, f, l.first, false);
            rem -= f;
        }
    }

    void on_trade(double p, double q, bool buyer_maker) {
        bool hits_bids = buyer_maker;
        bool hits_asks = !buyer_maker;
        double vol = q;
        for (auto& o : resting) {
            if (vol<=0) break;
            bool elig = (o.side=='B' && hits_bids && p<=o.px) ||
                        (o.side=='S' && hits_asks && p>=o.px);
            if (!elig) continue;
            if (o.queue_ahead>0) { double d=std::min(o.queue_ahead,vol); o.queue_ahead-=d; vol-=d; if(vol<=0) break; }
            if (o.queue_ahead<=0) {
                double f=std::min(o.qty,vol);
                pos.apply(o.side, f, o.px, maker_fee_bps);
                log_fill(o.side, f, o.px, true);
                o.qty-=f; vol-=f;
            }
        }
        resting.erase(std::remove_if(resting.begin(),resting.end(),
                      [](const Resting& r){return r.qty<=1e-12;}), resting.end());
    }

    void check_loss(double equity, double init) {
        if (!halted && (equity-init) <= -max_loss) {
            halted=true; cancel_all();
            emit("RISK:BLOCKED:max loss hit — trading halted");
        }
    }
};

using OnTickFn = StrategyOrder(*)(StrategyContext);
using OnInitFn = void(*)(int,double);
using NameFn   = const char*(*)();

struct Strategy {
    std::string id, name;
    void* handle=nullptr;
    OnTickFn on_tick=nullptr;

    double size=0.002, spread_bps=4.0, max_pos=0.02, thresh_bps=3.0;
    char   twap_side='B';

    std::deque<double> rets; double last_mid=0; int window=30; uint64_t tk=0;
};

static std::string ord_kind_to(int k) {
    switch (k) { case OK_Market: return "MARKET"; case OK_PostOnly: return "POST_ONLY";
                 default: return "LIMIT"; }
}

int main(int argc, char** argv) {
    std::string symbol="BTCUSDT", strat="mm", dylib, host_kind="spot";
    double cash=100000, size=0.002, spread_bps=4.0, max_pos=0.02;
    double maker_fee=0.0, taker_fee=5.0, max_notional=1e18, max_loss=1e18;
    bool futures=false;
    for (int i=1;i<argc;++i) {
        std::string a=argv[i];
        auto nxt=[&](double& d){ if(i+1<argc) d=atof(argv[++i]); };
        auto nxs=[&](std::string& s){ if(i+1<argc) s=argv[++i]; };
        if      (a=="--symbol")       nxs(symbol);
        else if (a=="--strategy")     nxs(strat);
        else if (a=="--dylib")        nxs(dylib);
        else if (a=="--cash")         nxt(cash);
        else if (a=="--size")         nxt(size);
        else if (a=="--spread-bps")   nxt(spread_bps);
        else if (a=="--max-position") nxt(max_pos);
        else if (a=="--maker-fee")    nxt(maker_fee);
        else if (a=="--taker-fee")    nxt(taker_fee);
        else if (a=="--max-notional") nxt(max_notional);
        else if (a=="--max-loss")     nxt(max_loss);
        else if (a=="--futures")      futures=true;
    }
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT,  on_signal);

    Strategy S; S.id=strat; S.size=size; S.spread_bps=spread_bps; S.max_pos=max_pos;
    if (strat=="custom" || !dylib.empty()) {
        S.handle = dlopen(dylib.c_str(), RTLD_NOW|RTLD_LOCAL);
        if (!S.handle) { emit(std::string("LOG:[ERROR] dlopen failed: ")+dlerror()); return 1; }
        S.on_tick = (OnTickFn)dlsym(S.handle, "on_tick");
        if (!S.on_tick) { emit("LOG:[ERROR] .dylib missing on_tick"); return 1; }
        if (auto nf=(NameFn)dlsym(S.handle,"strategy_name")) S.name = nf();
        else S.name = "Custom C++";
        if (auto inf=(OnInitFn)dlsym(S.handle,"on_init")) inf(0, cash);
        emit("LOG:loaded C++ strategy: " + S.name + "  (" + dylib + ")");
    } else {
        S.name = strat;
        emit("LOG:built-in strategy: " + strat);
    }

    std::string lsym=symbol; for (auto& c:lsym) c=(char)tolower(c);
    std::string host = futures ? "fstream.binance.com" : "stream.binance.com";
    std::string port = futures ? "443" : "9443";
    std::string target = "/stream?streams=" + lsym + "@depth20@100ms/" + lsym + "@trade";
    Gateway gw; gw.sym=symbol; gw.maker_fee_bps=maker_fee; gw.taker_fee_bps=taker_fee;
    gw.max_position=max_pos*50.0;
    gw.max_notional=max_notional; gw.max_loss=max_loss;

    Book bk;
    double init_eq=cash; uint64_t updates=0; std::deque<double> eqbuf;
    auto t_last_stat = std::chrono::steady_clock::now();

    auto run_builtin = [&](const Book& b){
        double mid=b.mid(); if(mid<=0) return;
        gw.cancel_all();
        if (S.id=="mm") {

            double half = mid * (S.spread_bps/1e4) / 2.0;
            double bid_px = std::min(b.best_bid(), mid - half);
            double ask_px = std::max(b.best_ask(), mid + half);
            bid_px = b.best_bid(); ask_px = b.best_ask();
            if (gw.pos.net <  S.max_pos) gw.post('B', S.size, bid_px, b, true);
            if (gw.pos.net > -S.max_pos) gw.post('S', S.size, ask_px, b, true);
        } else if (S.id=="momentum") {
            if (S.last_mid>0) S.rets.push_back(std::log(mid/S.last_mid));
            if ((int)S.rets.size()>S.window) S.rets.pop_front();
            double r=0; for(double x:S.rets) r+=x;
            if ((int)S.rets.size()>=S.window) {
                if (r >  S.thresh_bps/1e4 && gw.pos.net <  S.max_pos) gw.taker('B', S.size, b);
                if (r < -S.thresh_bps/1e4 && gw.pos.net > -S.max_pos) gw.taker('S', S.size, b);
            }
        } else if (S.id=="meanrev") {
            S.rets.push_back(mid);
            if ((int)S.rets.size()>S.window) S.rets.pop_front();
            if ((int)S.rets.size()>=S.window) {
                double m=0; for(double x:S.rets) m+=x; m/=S.rets.size();
                double sd=0; for(double x:S.rets) sd+=(x-m)*(x-m); sd=std::sqrt(sd/S.rets.size());
                double z = sd>0 ? (mid-m)/sd : 0;
                if (z> 2 && gw.pos.net > -S.max_pos) gw.taker('S', S.size, b);
                if (z<-2 && gw.pos.net <  S.max_pos) gw.taker('B', S.size, b);
                if (std::abs(z)<0.5 && gw.pos.net!=0)
                    gw.taker(gw.pos.net>0?'S':'B', std::abs(gw.pos.net), b);
            }
        } else if (S.id=="twap" || S.id=="meanrev_x") {
            if (S.tk % 20 == 0) gw.taker(S.twap_side, S.size, b);
        }
        S.last_mid = mid; S.tk++;
    };

    auto run_custom = [&](const Book& b){
        double mid=b.mid(); if(mid<=0) return;
        S.tk++;
        if (S.last_mid>0) { S.rets.push_back(std::log(mid/S.last_mid));
                            if((int)S.rets.size()>50) S.rets.pop_front(); }
        S.last_mid=mid;
        double vol=0; if(S.rets.size()>1){ double m=0; for(double x:S.rets)m+=x; m/=S.rets.size();
            for(double x:S.rets) vol+=(x-m)*(x-m); vol=std::sqrt(vol/S.rets.size()); }
        StrategyContext ctx{ b.best_bid(), b.best_ask(), mid, b.best_ask()-b.best_bid(),
            vol, S.tk, gw.pos.net, cash+gw.pos.realized, gw.pos.unreal(mid) };
        StrategyOrder o = S.on_tick(ctx);
        if (o.action==Action::None) return;
        gw.cancel_all();
        std::string kind = ord_kind_to(o.ord_type);

        auto route = [&](char side, double q, double px){
            if (kind=="MARKET") { gw.taker(side,q,b); return; }
            bool crosses = (side=='B' && b.best_ask()>0 && px>=b.best_ask())
                        || (side=='S' && b.best_bid()>0 && px<=b.best_bid());
            if (kind=="POST_ONLY")  gw.post(side,q,px,b,true);
            else if (crosses)       gw.taker(side,q,b);
            else                    gw.post(side,q,px,b,false);
        };
        if (o.action==Action::Buy) {
            double q = o.qty>0?o.qty:S.size; if(gw.pos.net+q>S.max_pos) q=std::max(0.0,S.max_pos-gw.pos.net);
            if(q<=0) return; route('B', q, o.price>0?o.price:b.best_ask());
        } else if (o.action==Action::Sell) {
            double q = o.qty>0?o.qty:S.size; if(gw.pos.net-q<-S.max_pos) q=std::max(0.0,S.max_pos+gw.pos.net);
            if(q<=0) return; route('S', q, o.price>0?o.price:b.best_bid());
        } else if (o.action==Action::Close) {
            if(gw.pos.net>0) gw.taker('S',gw.pos.net,b); else if(gw.pos.net<0) gw.taker('B',-gw.pos.net,b);
        }
    };

    int backoff = 1;
    while (!g_stop.load()) {
      WsClient ws;
      emit("LOG:connecting " + host + " ...");
      if (!ws.connect(host, port, target)) {
          emit("LOG:[reconnect] " + ws.last_error());
          for (int s=0; s<backoff && !g_stop.load(); ++s)
              std::this_thread::sleep_for(std::chrono::seconds(1));
          backoff = std::min(backoff*2, 30);
          continue;
      }
      ws.set_timeout(30);
      backoff = 1;
      emit("LOG:connected — paper trading " + symbol + " via " + S.name);

      std::string msg;
      while (!g_stop.load() && ws.read_message(msg)) {
        json j;
        try { j = json::parse(msg); } catch (...) { continue; }
        if (!j.contains("data")) continue;
        const auto& d = j["data"];
        std::string stream = j.value("stream","");

        if (stream.find("@trade") != std::string::npos) {
            try {
                double p = std::stod(d.value("p","0"));
                double q = std::stod(d.value("q","0"));
                bool   m = d.value("m", false);
                gw.on_trade(p, q, m);
            } catch (...) {}
            continue;
        }

        if (!d.contains("bids") || !d.contains("asks")) continue;
        bk.bids.clear(); bk.asks.clear();
        for (auto& lv : d["bids"]) { try { bk.bids.emplace_back(std::stod(lv[0].get<std::string>()),
                                                                 std::stod(lv[1].get<std::string>())); } catch(...){} }
        for (auto& lv : d["asks"]) { try { bk.asks.emplace_back(std::stod(lv[0].get<std::string>()),
                                                                 std::stod(lv[1].get<std::string>())); } catch(...){} }
        if (bk.bids.empty() || bk.asks.empty()) continue;

        if (S.on_tick) run_custom(bk); else run_builtin(bk);

        double mid=bk.mid();
        double eq = init_eq + gw.pos.realized + gw.pos.unreal(mid);
        gw.check_loss(eq, init_eq);
        eqbuf.push_back(eq); if(eqbuf.size()>5000) eqbuf.pop_front();

        emit("BA:" + f4(bk.best_bid()) + "," + f4(bk.best_ask()));
        { std::string b,a; int n=0;
          for (auto& l:bk.bids){ if(n++>=12)break; if(!b.empty())b+="|"; b+=f4(l.first)+":"+f6(l.second);} n=0;
          for (auto& l:bk.asks){ if(n++>=12)break; if(!a.empty())a+="|"; a+=f4(l.first)+":"+f6(l.second);}
          emit("DEPTH:"+b+";"+a); }
        emit("MID:" + f4(mid));
        emit("EQ:" + f4(eq));
        emit("POS:" + f6(gw.pos.net)+":"+f4(gw.pos.avg)+":"+f4(gw.pos.realized)+":"+f4(gw.pos.unreal(mid)));

        auto now=std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now-t_last_stat).count() >= 1000) {
            t_last_stat=now;
            double mr = gw.fills? (double)gw.maker_fills/gw.fills : 0.0;

            double mu=0,sd=0; int n=(int)eqbuf.size();
            if (n>2){ std::vector<double> r; for(int i=1;i<n;++i) r.push_back(eqbuf[i]-eqbuf[i-1]);
                for(double x:r)mu+=x; mu/=r.size(); for(double x:r)sd+=(x-mu)*(x-mu); sd=std::sqrt(sd/r.size()); }
            double sharpe = sd>0? mu/sd*std::sqrt(252*390*60):0.0;
            char buf[256];
            snprintf(buf,sizeof buf,"STAT:fills=%llu;orders=%llu;blocked=%llu;maker_ratio=%.4f;sharpe=%.4f",
                     (unsigned long long)gw.fills,(unsigned long long)gw.orders,
                     (unsigned long long)gw.blocked, mr, sharpe);
            emit(buf);
            if (!gw.halted) emit("RISK:OK:");
        }
        ++updates;
      }
      if (!g_stop.load())
          emit("LOG:feed dropped — reconnecting (position/PnL preserved)");
    }

    double mid=bk.mid();
    double eq = init_eq + gw.pos.realized + gw.pos.unreal(mid);
    emit("EQ:" + f4(eq));
    emit("LOG:session end — PnL $" + f4(eq-init_eq) + "  fills=" + std::to_string(gw.fills) +
         "  pos=" + f6(gw.pos.net));
    if (S.handle) dlclose(S.handle);
    return 0;
}
