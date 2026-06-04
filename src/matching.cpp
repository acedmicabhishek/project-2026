#include "hft_simulator/matching.hpp"
#include <chrono>
#include <algorithm>

namespace hft {

static uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(
            high_resolution_clock::now().time_since_epoch()).count());
}

LimitOrderBook& MatchingEngine::get_or_create_book(const std::string& symbol) {
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        auto [ins, _] = books_.emplace(symbol,
            std::make_unique<LimitOrderBook>(symbol));
        return *ins->second;
    }
    return *it->second;
}

LimitOrderBook* MatchingEngine::book(const std::string& symbol) {
    auto it = books_.find(symbol);
    return it != books_.end() ? it->second.get() : nullptr;
}

const LimitOrderBook* MatchingEngine::book(const std::string& symbol) const {
    auto it = books_.find(symbol);
    return it != books_.end() ? it->second.get() : nullptr;
}

MatchResult MatchingEngine::submit(Order order) {
    MatchResult result;
    if (order.qty == 0) {
        result.reject_reason = "qty=0";
        return result;
    }
    if (order.ts_ns == 0) order.ts_ns = now_ns();

    auto& lob = get_or_create_book(order.symbol);

    // FOK: check full fill possible before touching book
    if (order.type == OrdType::FOK) {
        uint32_t available = 0;
        if (order.side == Side::Buy) {
            for (auto* level = lob.best_ask_level(); level && available < order.qty; ) {
                available += level->total_qty;
                // Can't iterate further without complexity — approximate check
                break; // simplified: check best level only vs full qty
            }
        } else {
            for (auto* level = lob.best_bid_level(); level && available < order.qty; ) {
                available += level->total_qty;
                break;
            }
        }
        // Full scan for FOK
        uint32_t fillable = 0;
        if (order.side == Side::Buy) {
            for (auto& d : lob.ask_depth(999)) {
                if (order.type == OrdType::Limit && d.price > order.price) break;
                fillable += d.qty;
                if (fillable >= order.qty) break;
            }
        } else {
            for (auto& d : lob.bid_depth(999)) {
                if (order.type == OrdType::Limit && d.price < order.price) break;
                fillable += d.qty;
                if (fillable >= order.qty) break;
            }
        }
        if (fillable < order.qty) {
            order.status = OrdStatus::Rejected;
            result.reject_reason = "FOK: insufficient liquidity";
            return result;
        }
    }

    // Store order
    uint64_t oid = order.id;
    orders_[oid] = std::move(order);
    Order& stored = orders_[oid];
    order_symbol_[oid] = stored.symbol;

    result.accepted = true;

    // Market or limit crossing: match against book
    if (stored.type == OrdType::Market ||
        stored.type == OrdType::IOC    ||
        stored.type == OrdType::FOK    ||
        (stored.type == OrdType::Limit &&
         ((stored.side == Side::Buy  && lob.best_ask() && stored.price >= *lob.best_ask()) ||
          (stored.side == Side::Sell && lob.best_bid() && stored.price <= *lob.best_bid()))))
    {
        result.fills = match_against_book(stored, lob);
    }

    // After matching: if limit and not fully filled, add to book
    if (!stored.is_done()) {
        if (stored.type == OrdType::Limit) {
            lob.add(stored);
            if (on_add) on_add(stored);
        } else {
            // Market / IOC / FOK remainder: cancel
            stored.status = OrdStatus::Cancelled;
            if (on_cancel) on_cancel(stored.id);
        }
    }

    fire_book_update(lob, stored.ts_ns);
    return result;
}

std::vector<Fill> MatchingEngine::match_against_book(Order& taker, LimitOrderBook& lob) {
    std::vector<Fill> fills;

    while (taker.leaves_qty() > 0) {
        PriceLevel* maker_level = (taker.side == Side::Buy)
            ? lob.best_ask_level()
            : lob.best_bid_level();

        if (!maker_level || maker_level->orders.empty()) break;

        // Price check for limit orders
        if (taker.type == OrdType::Limit) {
            if (taker.side == Side::Buy  && maker_level->price > taker.price) break;
            if (taker.side == Side::Sell && maker_level->price < taker.price) break;
        }

        double fill_price = maker_level->price;

        // FIFO: consume orders from front of queue
        while (!maker_level->orders.empty() && taker.leaves_qty() > 0) {
            Order* maker = maker_level->orders.front();
            uint32_t fill_qty = std::min(taker.leaves_qty(), maker->leaves_qty());

            // Apply fill to both sides
            maker->filled_qty += fill_qty;
            taker.filled_qty  += fill_qty;
            maker_level->total_qty -= fill_qty;

            if (maker->leaves_qty() == 0) {
                maker->status = OrdStatus::Filled;
                maker_level->orders.pop_front();
                lob.erase_filled(maker->id);   // keep id_map consistent
            } else {
                maker->status = OrdStatus::PartialFill;
            }

            if (taker.filled_qty == taker.qty) {
                taker.status = OrdStatus::Filled;
            } else {
                taker.status = OrdStatus::PartialFill;
            }

            Fill f;
            f.maker_id  = maker->id;
            f.taker_id  = taker.id;
            f.price     = fill_price;
            f.qty       = fill_qty;
            f.aggressor = taker.side;
            f.ts_ns     = taker.ts_ns;
            fills.push_back(f);

            if (on_fill) on_fill(f);
        }

        lob.remove_level_if_empty(
            (taker.side == Side::Buy) ? Side::Sell : Side::Buy,
            fill_price);
    }

    return fills;
}

bool MatchingEngine::cancel(uint64_t order_id) {
    auto sym_it = order_symbol_.find(order_id);
    if (sym_it == order_symbol_.end()) return false;

    auto* lob = book(sym_it->second);
    if (!lob) return false;

    bool cancelled = lob->cancel(order_id);
    if (cancelled) {
        if (on_cancel) on_cancel(order_id);
        order_symbol_.erase(sym_it);
        fire_book_update(*lob, now_ns());
    }
    return cancelled;
}

void MatchingEngine::fire_book_update(const LimitOrderBook& lob, uint64_t ts_ns) {
    if (on_book_update) on_book_update(lob.snapshot(ts_ns));
}

} // namespace hft
