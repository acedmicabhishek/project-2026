
#pragma once
#include <cstdint>

struct StrategyContext {
    double   bid;
    double   ask;
    double   mid;
    double   spread;
    double   volatility;
    uint64_t tick_num;
    double   position;
    double   cash;
    double   unrealised_pnl;
};

enum class Action : int {
    None = 0,
    Buy  = 1,
    Sell = 2,
    Close= 3,
};

enum OrderKind : int {
    OK_Limit = 1, OK_Market = 0, OK_IOC = 2, OK_FOK = 3,
    OK_PostOnly = 4, OK_Stop = 5, OK_StopLimit = 6, OK_Iceberg = 7,
};

struct StrategyOrder {
    Action action;
    double qty;
    double price;
    int    ord_type{OK_Limit};
    double stop_price{0.0};
    double display_qty{0.0};
};

extern "C" StrategyOrder on_tick(StrategyContext ctx);

extern "C" const char* strategy_name();
