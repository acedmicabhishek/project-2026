#pragma once

#include "hft_simulator/order.hpp"
#include <map>
#include <list>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>

namespace hft {

struct PriceLevel {
    double   price{0.0};
    uint32_t total_qty{0};
    std::list<Order*> orders;
};

struct DepthLevel {
    double   price;
    uint32_t qty;
    int      order_count;
};

class LimitOrderBook {
public:
    explicit LimitOrderBook(std::string symbol);

    void add(Order& order);
    bool cancel(uint64_t order_id);
    bool modify(uint64_t order_id, uint32_t new_qty);

    std::optional<double> best_bid() const noexcept;
    std::optional<double> best_ask() const noexcept;
    std::optional<uint32_t> best_bid_qty() const noexcept;
    std::optional<uint32_t> best_ask_qty() const noexcept;
    double spread() const noexcept;
    double mid()    const noexcept;
    uint32_t qty_at(double price) const noexcept;

    std::vector<DepthLevel> bid_depth(int n = 5) const;
    std::vector<DepthLevel> ask_depth(int n = 5) const;

    BookSnapshot snapshot(uint64_t ts_ns = 0) const;

    const std::string& symbol() const noexcept { return symbol_; }
    bool empty() const noexcept { return bids_.empty() && asks_.empty(); }

    PriceLevel* best_bid_level() noexcept;
    PriceLevel* best_ask_level() noexcept;
    void remove_level_if_empty(Side side, double price);

    void erase_filled(uint64_t order_id) noexcept { id_map_.erase(order_id); }

private:
    struct OrderLocation {
        Side side;
        double price;
        std::list<Order*>::iterator list_it;
    };

    std::string symbol_;

    std::map<double, PriceLevel, std::greater<double>> bids_;

    std::map<double, PriceLevel> asks_;

    std::unordered_map<uint64_t, OrderLocation> id_map_;
};

}
