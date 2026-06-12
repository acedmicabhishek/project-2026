#pragma once

#include <vector>
#include <string>
#include <unordered_map>

namespace hft {

using ParamMap = std::unordered_map<std::string, double>;

struct ParamSpec {
    const char* key;
    const char* label;
    double min;
    double max;
    double def;
    double step;
    bool   integer;
};

inline const std::vector<ParamSpec>& param_spec(const std::string& strat_id) {
    static const std::vector<ParamSpec> empty;
    static const std::unordered_map<std::string, std::vector<ParamSpec>> specs = {
        {"mm", {
            {"gamma", "Gamma (Inventory Risk)", 1.0,  500.0, 150.0, 1.0,  false},
            {"kappa", "Kappa (Order Intensity)", 0.1, 10.0,  1.5,   0.1,  false},
        }},
        {"momentum", {
            {"lookback",   "Lookback (ticks)",   5.0,  100.0, 30.0,  1.0, true},
            {"thresh_bps", "Threshold (bps)",     1.0,  30.0,  3.0,   1.0, false},
            {"size",       "Order Size",          10.0, 200.0, 50.0,  5.0, true},
            {"max_pos",    "Max Position",        50.0, 500.0, 200.0, 10.0, true},
        }},
        {"mean_rev", {
            {"window",  "BB Window (ticks)",  10.0, 150.0, 50.0,  1.0, true},
            {"mult",    "Band Mult (sigma)",  0.5,  4.0,   2.0,   0.1, false},
            {"size",    "Order Size",         10.0, 200.0, 40.0,  5.0, true},
            {"max_pos", "Max Position",       50.0, 400.0, 160.0, 10.0, true},
        }},
        {"rsi", {
            {"period",     "RSI Period",   5.0,  40.0, 14.0, 1.0, true},
            {"oversold",   "Oversold",     10.0, 45.0, 30.0, 1.0, false},
            {"overbought", "Overbought",   55.0, 90.0, 70.0, 1.0, false},
            {"size",       "Order Size",   10.0, 150.0, 30.0, 5.0, true},
        }},
        {"breakout", {
            {"channel",  "Donchian Channel", 5.0,  80.0,  20.0,  1.0, true},
            {"stop_pct", "Stop Loss (%)",    0.1,  3.0,   0.5,   0.1, false},
            {"size",     "Order Size",       10.0, 150.0, 25.0,  5.0, true},
            {"max_pos",  "Max Position",     50.0, 300.0, 100.0, 10.0, true},
        }},
    };
    auto it = specs.find(strat_id);
    return it == specs.end() ? empty : it->second;
}

inline ParamMap& fill_defaults(const std::string& strat_id, ParamMap& m) {
    for (const auto& ps : param_spec(strat_id))
        if (!m.count(ps.key)) m[ps.key] = ps.def;
    return m;
}

inline double param_or_default(const std::string& strat_id, const ParamMap& m,
                               const std::string& key) {
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    for (const auto& ps : param_spec(strat_id))
        if (key == ps.key) return ps.def;
    return 0.0;
}

}
