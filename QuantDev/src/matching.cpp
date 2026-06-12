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

    if (order.type == OrdType::Stop || order.type == OrdType::StopLimit) {
        stops_[order.symbol].push_back(order);
        result.accepted = true;
        check_stops(order.symbol);
        return result;
    }

    if (order.type == OrdType::PostOnly) {
        bool crosses = (order.side == Side::Buy  && lob.best_ask() && order.price >= *lob.best_ask())
                    || (order.side == Side::Sell && lob.best_bid() && order.price <= *lob.best_bid());
        if (crosses) { order.status = OrdStatus::Rejected; result.reject_reason = "post-only would cross"; return result; }
        order.type = OrdType::Limit;
    }

    uint32_t ice_reserve = 0, ice_display = 0;
    bool is_ice = (order.type == OrdType::Iceberg);
    if (is_ice) {
        ice_display = order.display_qty > 0 ? std::min(order.display_qty, order.qty) : order.qty;
        ice_reserve = order.qty - ice_display;
        order.qty   = ice_display;
        order.type  = OrdType::Limit;
    }

    if (order.type == OrdType::FOK) {
        uint32_t available = 0;
        if (order.side == Side::Buy) {
            for (auto* level = lob.best_ask_level(); level && available < order.qty; ) {
                available += level->total_qty;

                break;
            }
        } else {
            for (auto* level = lob.best_bid_level(); level && available < order.qty; ) {
                available += level->total_qty;
                break;
            }
        }

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

    uint64_t oid = order.id;
    orders_[oid] = std::move(order);
    Order& stored = orders_[oid];
    order_symbol_[oid] = stored.symbol;

    result.accepted = true;

    if (stored.type == OrdType::Market ||
        stored.type == OrdType::IOC    ||
        stored.type == OrdType::FOK    ||
        (stored.type == OrdType::Limit &&
         ((stored.side == Side::Buy  && lob.best_ask() && stored.price >= *lob.best_ask()) ||
          (stored.side == Side::Sell && lob.best_bid() && stored.price <= *lob.best_bid()))))
    {
        result.fills = match_against_book(stored, lob);
    }

    if (!stored.is_done()) {
        if (stored.type == OrdType::Limit) {
            lob.add(stored);
            if (on_add) on_add(stored);
        } else {

            stored.status = OrdStatus::Cancelled;
            if (on_cancel) on_cancel(stored.id);
        }
    }

    if (is_ice && ice_reserve > 0 && !stored.is_done() && stored.type == OrdType::Limit)
        icebergs_[oid] = { ice_reserve, ice_display, stored.symbol, stored.side, stored.price, stored.is_bot };

    fire_book_update(lob, stored.ts_ns);
    check_stops(stored.symbol);
    return result;
}

std::vector<Fill> MatchingEngine::match_against_book(Order& taker, LimitOrderBook& lob) {
    std::vector<Fill> fills;

    while (taker.leaves_qty() > 0) {
        PriceLevel* maker_level = (taker.side == Side::Buy)
            ? lob.best_ask_level()
            : lob.best_bid_level();

        if (!maker_level || maker_level->orders.empty()) break;

        if (taker.type == OrdType::Limit) {
            if (taker.side == Side::Buy  && maker_level->price > taker.price) break;
            if (taker.side == Side::Sell && maker_level->price < taker.price) break;
        }

        double fill_price = maker_level->price;

        while (!maker_level->orders.empty() && taker.leaves_qty() > 0) {
            Order* maker = maker_level->orders.front();
            uint32_t fill_qty = std::min(taker.leaves_qty(), maker->leaves_qty());

            maker->filled_qty += fill_qty;
            taker.filled_qty  += fill_qty;
            maker_level->total_qty -= fill_qty;

            if (maker->leaves_qty() == 0) {
                maker->status = OrdStatus::Filled;
                uint64_t filled_maker_id = maker->id;
                maker_level->orders.pop_front();
                lob.erase_filled(filled_maker_id);
                replenish_iceberg(filled_maker_id);
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
            f.maker_is_bot = maker->is_bot;
            f.taker_is_bot = taker.is_bot;
            f.symbol    = lob.symbol();
            fills.push_back(f);

            if (on_fill) on_fill(f);
        }

        lob.remove_level_if_empty(
            (taker.side == Side::Buy) ? Side::Sell : Side::Buy,
            fill_price);
    }

    return fills;
}

void MatchingEngine::check_stops(const std::string& symbol) {
    auto it = stops_.find(symbol);
    if (it == stops_.end() || it->second.empty()) return;
    auto* lob = book(symbol);
    if (!lob) return;
    auto bb = lob->best_bid(); auto ba = lob->best_ask();

    std::vector<Order> triggered;
    auto& vec = it->second;
    for (size_t i = 0; i < vec.size(); ) {
        const Order& s = vec[i];

        bool trig = (s.side == Side::Buy)  ? (ba && *ba >= s.stop_price)
                                           : (bb && *bb <= s.stop_price);
        if (trig) { triggered.push_back(s); vec.erase(vec.begin() + i); }
        else ++i;
    }
    for (Order s : triggered) {
        s.type = (s.type == OrdType::StopLimit) ? OrdType::Limit : OrdType::Market;
        s.stop_price = 0.0;
        s.filled_qty = 0; s.status = OrdStatus::New;
        submit(s);
    }
}

void MatchingEngine::replenish_iceberg(uint64_t maker_id) {
    auto it = icebergs_.find(maker_id);
    if (it == icebergs_.end()) return;
    IcebergInfo info = it->second;
    icebergs_.erase(it);
    if (info.reserve == 0) return;

    uint32_t clip = std::min(info.display, info.reserve);
    info.reserve -= clip;

    uint64_t nid = next_order_id();
    Order o;
    o.id = nid; o.symbol = info.symbol; o.side = info.side;
    o.type = OrdType::Limit; o.price = info.price; o.qty = clip;
    o.is_bot = info.is_bot; o.ts_ns = now_ns();

    orders_[nid] = o;
    Order& stored = orders_[nid];
    order_symbol_[nid] = info.symbol;
    auto& lob = get_or_create_book(info.symbol);
    lob.add(stored);
    if (on_add) on_add(stored);
    if (info.reserve > 0) icebergs_[nid] = info;
}

bool MatchingEngine::cancel(uint64_t order_id) {

    for (auto& [sym, vec] : stops_) {
        for (size_t i = 0; i < vec.size(); ++i) {
            if (vec[i].id == order_id) {
                vec.erase(vec.begin() + i);
                if (on_cancel) on_cancel(order_id);
                return true;
            }
        }
    }
    icebergs_.erase(order_id);

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

}
