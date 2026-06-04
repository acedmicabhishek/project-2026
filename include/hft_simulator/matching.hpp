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
    // Callbacks — set before calling submit()
    std::function<void(const Fill&)>         on_fill;
    std::function<void(const Order&)>        on_add;
    std::function<void(uint64_t)>            on_cancel;
    std::function<void(const BookSnapshot&)> on_book_update;

    // Submit order; engine takes ownership via internal storage.
    // Returns filled fills for this order (synchronous).
    MatchResult submit(Order order);

    bool cancel(uint64_t order_id);

    // Returns nullptr if symbol not seen yet
    LimitOrderBook* book(const std::string& symbol);
    const LimitOrderBook* book(const std::string& symbol) const;

    // Ensure book exists for symbol (created lazily on first submit)
    LimitOrderBook& get_or_create_book(const std::string& symbol);

    uint64_t next_order_id() noexcept { return ++order_seq_; }

private:
    std::vector<Fill> match_against_book(Order& taker, LimitOrderBook& lob);
    void fire_book_update(const LimitOrderBook& lob, uint64_t ts_ns);

    std::unordered_map<std::string, std::unique_ptr<LimitOrderBook>> books_;

    // Order storage: engine owns all live orders
    std::unordered_map<uint64_t, Order> orders_;
    // Mapping: order_id → symbol (to route cancel to correct book)
    std::unordered_map<uint64_t, std::string> order_symbol_;

    uint64_t order_seq_{0};
};

} // namespace hft
