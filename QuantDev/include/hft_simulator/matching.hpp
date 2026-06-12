#pragma once

#include "hft_simulator/order.hpp"
#include "hft_simulator/orderbook.hpp"
#include <functional>
#include <unordered_map>
#include <memory>
#include <vector>
#include <deque>

namespace hft {

struct MatchResult {
    std::vector<Fill> fills;
    bool accepted{false};
    std::string reject_reason;
};

class MatchingEngine {
public:

    std::function<void(const Fill&)>         on_fill;
    std::function<void(const Order&)>        on_add;
    std::function<void(uint64_t)>            on_cancel;
    std::function<void(const BookSnapshot&)> on_book_update;

    MatchResult submit(Order order);

    bool cancel(uint64_t order_id);

    LimitOrderBook* book(const std::string& symbol);
    const LimitOrderBook* book(const std::string& symbol) const;

    LimitOrderBook& get_or_create_book(const std::string& symbol);

    uint64_t next_order_id() noexcept { return ++order_seq_; }

private:
    std::vector<Fill> match_against_book(Order& taker, LimitOrderBook& lob);
    void fire_book_update(const LimitOrderBook& lob, uint64_t ts_ns);

    void check_stops(const std::string& symbol);

    void replenish_iceberg(uint64_t maker_id);

    std::unordered_map<std::string, std::unique_ptr<LimitOrderBook>> books_;

    std::unordered_map<std::string, std::vector<Order>> stops_;

    struct IcebergInfo { uint32_t reserve; uint32_t display; std::string symbol;
                         Side side; double price; bool is_bot; };
    std::unordered_map<uint64_t, IcebergInfo> icebergs_;

    std::unordered_map<uint64_t, Order> orders_;

    std::unordered_map<uint64_t, std::string> order_symbol_;

    uint64_t order_seq_{0};
};

}
