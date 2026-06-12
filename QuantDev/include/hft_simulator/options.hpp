#pragma once

#include <cmath>
#include <vector>
#include <span>
#include <mdspan>

namespace hft {

enum GreekCol { G_PRICE = 0, G_DELTA, G_GAMMA, G_VEGA, G_THETA, G_NCOLS };

inline double norm_cdf(double x) { return 0.5 * std::erfc(-x * 0.7071067811865476); }
inline double norm_pdf(double x) { return 0.3989422804014327 * std::exp(-0.5 * x * x); }

struct Greeks {
    double price{0}, delta{0}, gamma{0}, vega{0}, theta{0};
};

inline Greeks bs_greeks(double S, double K, double T, double r, double sigma, bool call) {
    if (T <= 0.0 || sigma <= 0.0 || S <= 0.0) {
        double intr = call ? std::max(0.0, S - K) : std::max(0.0, K - S);
        double d = call ? (S > K ? 1.0 : 0.0) : (S < K ? -1.0 : 0.0);
        return { intr, d, 0, 0, 0 };
    }
    double sqrtT = std::sqrt(T);
    double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * sqrtT);
    double d2 = d1 - sigma * sqrtT;
    double Nd1 = norm_cdf(d1), Nd2 = norm_cdf(d2), nd1 = norm_pdf(d1);
    double disc = std::exp(-r * T);
    Greeks g;
    g.price = call ? S * Nd1 - K * disc * Nd2
                   : K * disc * norm_cdf(-d2) - S * norm_cdf(-d1);
    g.delta = call ? Nd1 : Nd1 - 1.0;
    g.gamma = nd1 / (S * sigma * sqrtT);
    g.vega  = S * nd1 * sqrtT;
    g.theta = call ? (-S * nd1 * sigma / (2 * sqrtT) - r * K * disc * Nd2)
                   : (-S * nd1 * sigma / (2 * sqrtT) + r * K * disc * norm_cdf(-d2));
    return g;
}

inline double bs_price(double S, double K, double T, double r, double sigma, bool call) {
    return bs_greeks(S, K, T, r, sigma, call).price;
}

inline std::vector<double>
bs_greeks_surface(double S, std::span<const double> strikes,
                  double T, double r, double sigma, bool call) {
    std::vector<double> grid(strikes.size() * G_NCOLS);
    std::mdspan g{grid.data(), strikes.size(), std::size_t(G_NCOLS)};
    for (std::size_t i = 0; i < strikes.size(); ++i) {
        [[assume(strikes[i] > 0.0)]];
        Greeks gr = bs_greeks(S, strikes[i], T, r, sigma, call);
        g[i, G_PRICE] = gr.price; g[i, G_DELTA] = gr.delta; g[i, G_GAMMA] = gr.gamma;
        g[i, G_VEGA]  = gr.vega;  g[i, G_THETA] = gr.theta;
    }
    return grid;
}

inline auto greeks_view(std::vector<double>& grid, std::size_t n_strikes) {
    return std::mdspan{grid.data(), n_strikes, std::size_t(G_NCOLS)};
}

inline double implied_vol(double mkt_price, double S, double K, double T, double r, bool call) {
    double lo = 1e-4, hi = 5.0;
    for (int i = 0; i < 100; ++i) {
        double mid = 0.5 * (lo + hi);
        double p = bs_price(S, K, T, r, mid, call);
        if (p > mkt_price) hi = mid; else lo = mid;
        if (hi - lo < 1e-6) break;
    }
    return 0.5 * (lo + hi);
}

}
