#pragma once

#include "hft_simulator/order.hpp"
#include "hft_simulator/matching.hpp"
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace hft {

// ─── Generic tick event (normalized from any feed) ───────────────────────────

struct TickEvent {
    uint64_t    ts_ns{0};
    std::string symbol;
    double      price{0.0};
    double      bid{0.0};
    double      ask{0.0};
    uint32_t    size{0};          // trade size (0 if quote-only)
    bool        is_trade{false};  // true = print/trade, false = quote update

    double mid() const noexcept { return (bid + ask) / 2.0; }
};

using TickCallback = std::function<void(const TickEvent&)>;

// ─── ITCH 5.0 Message Structs ─────────────────────────────────────────────────

#pragma pack(push, 1)

struct ITCHHeader {
    uint16_t length;    // message length (big-endian)
    char     msg_type;
};

struct ITCHAddOrder {         // msg_type = 'A'
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];   // 48-bit nanoseconds since midnight
    uint64_t order_ref;
    char     side;            // 'B' or 'S'
    uint32_t shares;
    char     stock[8];
    uint32_t price;           // price * 10000
};

struct ITCHDeleteOrder {      // msg_type = 'D'
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref;
};

struct ITCHOrderCancelled {   // msg_type = 'X'
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    uint32_t cancelled_shares;
};

struct ITCHTrade {            // msg_type = 'P'
    char     msg_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    char     side;
    uint32_t shares;
    char     stock[8];
    uint32_t price;
    uint64_t match_number;
};

#pragma pack(pop)

// ─── ITCH Parser ─────────────────────────────────────────────────────────────

class ITCHParser {
public:
    // Callbacks for each message type
    std::function<void(const ITCHAddOrder&,      uint64_t ts_ns)> on_add_order;
    std::function<void(const ITCHDeleteOrder&,   uint64_t ts_ns)> on_delete_order;
    std::function<void(const ITCHOrderCancelled&,uint64_t ts_ns)> on_cancel_order;
    std::function<void(const ITCHTrade&,          uint64_t ts_ns)> on_trade;

    // Parse a raw buffer (one or more length-prefixed ITCH messages)
    void parse(const uint8_t* buf, size_t len);

    // Parse a single .bin file (MoldUDP64 encapsulated ITCH)
    void parse_file(const std::string& path);

    // Convert ITCH 48-bit timestamp to full nanoseconds
    static uint64_t decode_ts(const uint8_t ts[6]);
    // Convert ITCH price (fixed-point *10000) to double
    static double   decode_price(uint32_t raw) { return raw / 10000.0; }
    // Convert ITCH stock field to trimmed string
    static std::string decode_symbol(const char stock[8]);

private:
    size_t parse_message(const uint8_t* msg, size_t available);
};

// ─── CSV Tick Replay ─────────────────────────────────────────────────────────

class CsvTickReplay {
public:
    // CSV format: ts_ns,symbol,price,bid,ask,size,is_trade
    void load(const std::string& path);

    // Replay all ticks invoking callback. speed_multiplier = 0 → max speed.
    void play(TickCallback cb, double speed_multiplier = 0.0);

    size_t tick_count() const { return ticks_.size(); }
    const std::vector<TickEvent>& ticks() const { return ticks_; }

private:
    std::vector<TickEvent> ticks_;
};

// ─── Synthetic Market Data Generator ─────────────────────────────────────────
// Upgraded from original MarketDataGenerator: proper GBM + OU process

struct SyntheticFeedParams {
    std::string symbol    = "SYNTH";
    double initial_price  = 100.0;
    double mu             = 0.0;        // drift (per tick)
    double sigma          = 0.0005;     // volatility (per tick)
    double tick_size      = 0.01;
    double spread_ticks   = 2.0;        // bid/ask spread in ticks
    unsigned seed         = 0;
};

struct OUParams {
    double theta = 0.1;
    double mu    = 0.0;
    double sigma = 0.01;
};

class SyntheticFeed {
public:
    using Params = SyntheticFeedParams;

    // GBM: dS = μS dt + σS dW
    explicit SyntheticFeed(Params p = {});
    ~SyntheticFeed();

    TickEvent next_tick();
    std::vector<TickEvent> generate(size_t n);

    // OU mean-reverting process (for spread/pair simulation)
    // dX = θ(μ - X)dt + σ dW
    static std::vector<double> generate_ou(size_t n, OUParams p, unsigned seed = 0);

private:
    Params  params_;
    double  price_;
    uint64_t ts_ns_{0};
    // mt19937 with normal distribution for GBM
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── OU Spread Feed (two correlated assets) ───────────────────────────────────

class OUSpreadFeed {
public:
    OUSpreadFeed(std::string sym_a, std::string sym_b,
                  double initial_price,
                  double theta, double mu_spread, double sigma_spread,
                  double sigma_price, double tick_size = 0.01,
                  double spread_ticks = 2.0, double dt = 1.0,
                  unsigned seed = 0);
    ~OUSpreadFeed();

    // Returns (tick_a, tick_b)
    std::pair<TickEvent, TickEvent> next();

private:
    std::string sym_a_, sym_b_;
    double price_a_, price_b_, spread_;
    double theta_, mu_spread_, sigma_spread_, sigma_price_;
    double tick_size_, half_spread_, dt_;
    uint64_t ts_ns_{0};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hft
