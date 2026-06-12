#include "hft_simulator/orderbook.hpp"
#include <stdexcept>

namespace hft {

LimitOrderBook::LimitOrderBook(std::string symbol)
    : symbol_(std::move(symbol)) {}

void LimitOrderBook::add(Order& order) {
    if (order.is_done()) return;

    if (order.side == Side::Buy) {
        auto& level = bids_[order.price];
        level.price = order.price;
        level.total_qty += order.leaves_qty();
        level.orders.push_back(&order);
        auto it = std::prev(level.orders.end());
        id_map_[order.id] = {Side::Buy, order.price, it};
    } else {
        auto& level = asks_[order.price];
        level.price = order.price;
        level.total_qty += order.leaves_qty();
        level.orders.push_back(&order);
        auto it = std::prev(level.orders.end());
        id_map_[order.id] = {Side::Sell, order.price, it};
    }
}

bool LimitOrderBook::cancel(uint64_t order_id) {
    auto mit = id_map_.find(order_id);
    if (mit == id_map_.end()) return false;

    auto& loc = mit->second;
    Order* ord = *loc.list_it;

    if (loc.side == Side::Buy) {
        auto bit = bids_.find(loc.price);
        if (bit != bids_.end()) {
            bit->second.total_qty -= ord->leaves_qty();
            bit->second.orders.erase(loc.list_it);
            if (bit->second.orders.empty()) bids_.erase(bit);
        }
    } else {
        auto ait = asks_.find(loc.price);
        if (ait != asks_.end()) {
            ait->second.total_qty -= ord->leaves_qty();
            ait->second.orders.erase(loc.list_it);
            if (ait->second.orders.empty()) asks_.erase(ait);
        }
    }

    ord->status = OrdStatus::Cancelled;
    id_map_.erase(mit);
    return true;
}

bool LimitOrderBook::modify(uint64_t order_id, uint32_t new_qty) {
    auto mit = id_map_.find(order_id);
    if (mit == id_map_.end()) return false;

    Order* ord = *mit->second.list_it;
    if (new_qty >= ord->qty) {

        uint32_t delta = new_qty - ord->qty;
        ord->qty = new_qty;
        if (ord->side == Side::Buy) {
            bids_[ord->price].total_qty += delta;
        } else {
            asks_[ord->price].total_qty += delta;
        }
        return true;
    }

    uint32_t old_leaves = ord->leaves_qty();
    ord->qty = new_qty;
    uint32_t new_leaves = ord->leaves_qty();
    int32_t delta = static_cast<int32_t>(new_leaves) - static_cast<int32_t>(old_leaves);

    if (ord->side == Side::Buy) {
        bids_[ord->price].total_qty += delta;
    } else {
        asks_[ord->price].total_qty += delta;
    }

    if (ord->leaves_qty() == 0) cancel(order_id);
    return true;
}

uint32_t LimitOrderBook::qty_at(double price) const noexcept {
    auto bit = bids_.find(price);
    if (bit != bids_.end()) return bit->second.total_qty;
    auto ait = asks_.find(price);
    if (ait != asks_.end()) return ait->second.total_qty;
    return 0;
}

std::optional<double> LimitOrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<double> LimitOrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<uint32_t> LimitOrderBook::best_bid_qty() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->second.total_qty;
}

std::optional<uint32_t> LimitOrderBook::best_ask_qty() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->second.total_qty;
}

double LimitOrderBook::spread() const noexcept {
    auto b = best_bid();
    auto a = best_ask();
    if (!b || !a) return 0.0;
    return *a - *b;
}

double LimitOrderBook::mid() const noexcept {
    auto b = best_bid();
    auto a = best_ask();
    if (!b || !a) return 0.0;
    return (*b + *a) / 2.0;
}

std::vector<DepthLevel> LimitOrderBook::bid_depth(int n) const {
    std::vector<DepthLevel> depth;
    depth.reserve(n);
    for (auto& [price, level] : bids_) {
        if (static_cast<int>(depth.size()) >= n) break;
        depth.push_back({price, level.total_qty,
                          static_cast<int>(level.orders.size())});
    }
    return depth;
}

std::vector<DepthLevel> LimitOrderBook::ask_depth(int n) const {
    std::vector<DepthLevel> depth;
    depth.reserve(n);
    for (auto& [price, level] : asks_) {
        if (static_cast<int>(depth.size()) >= n) break;
        depth.push_back({price, level.total_qty,
                          static_cast<int>(level.orders.size())});
    }
    return depth;
}

BookSnapshot LimitOrderBook::snapshot(uint64_t ts_ns) const {
    BookSnapshot snap;
    snap.symbol = symbol_;
    snap.ts_ns  = ts_ns;
    if (auto b = best_bid()) { snap.best_bid = *b; snap.bid_sz = *best_bid_qty(); }
    if (auto a = best_ask()) { snap.best_ask = *a; snap.ask_sz = *best_ask_qty(); }
    return snap;
}

PriceLevel* LimitOrderBook::best_bid_level() noexcept {
    if (bids_.empty()) return nullptr;
    return &bids_.begin()->second;
}

PriceLevel* LimitOrderBook::best_ask_level() noexcept {
    if (asks_.empty()) return nullptr;
    return &asks_.begin()->second;
}

void LimitOrderBook::remove_level_if_empty(Side side, double price) {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end() && it->second.orders.empty())
            bids_.erase(it);
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end() && it->second.orders.empty())
            asks_.erase(it);
    }
}

}
