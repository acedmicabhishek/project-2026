#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "hft_simulator/order.hpp"
#include "hft_simulator/orderbook.hpp"
#include "hft_simulator/matching.hpp"
#include "hft_simulator/risk.hpp"
#include "hft_simulator/metrics.hpp"
#include "hft_simulator/feed.hpp"

namespace py = pybind11;
using namespace hft;

PYBIND11_MODULE(quantsim_core, m) {
    m.doc() = "QuantSim C++ core: LOB matching engine, risk, feed handlers";

    // ── Enums ─────────────────────────────────────────────────────────────────

    py::enum_<Side>(m, "Side")
        .value("Buy",  Side::Buy)
        .value("Sell", Side::Sell)
        .export_values();

    py::enum_<OrdType>(m, "OrdType")
        .value("Market", OrdType::Market)
        .value("Limit",  OrdType::Limit)
        .value("IOC",    OrdType::IOC)
        .value("FOK",    OrdType::FOK)
        .export_values();

    py::enum_<OrdStatus>(m, "OrdStatus")
        .value("New",         OrdStatus::New)
        .value("PartialFill", OrdStatus::PartialFill)
        .value("Filled",      OrdStatus::Filled)
        .value("Cancelled",   OrdStatus::Cancelled)
        .value("Rejected",    OrdStatus::Rejected)
        .export_values();

    // ── Order / Fill / BookSnapshot ───────────────────────────────────────────

    py::class_<Order>(m, "Order")
        .def(py::init<>())
        .def_readwrite("id",         &Order::id)
        .def_readwrite("symbol",     &Order::symbol)
        .def_readwrite("side",       &Order::side)
        .def_readwrite("type",       &Order::type)
        .def_readwrite("price",      &Order::price)
        .def_readwrite("qty",        &Order::qty)
        .def_readwrite("filled_qty", &Order::filled_qty)
        .def_readwrite("status",     &Order::status)
        .def_readwrite("ts_ns",      &Order::ts_ns)
        .def("leaves_qty",           &Order::leaves_qty)
        .def("is_done",              &Order::is_done);

    py::class_<Fill>(m, "Fill")
        .def(py::init<>())
        .def_readwrite("maker_id",  &Fill::maker_id)
        .def_readwrite("taker_id",  &Fill::taker_id)
        .def_readwrite("price",     &Fill::price)
        .def_readwrite("qty",       &Fill::qty)
        .def_readwrite("aggressor", &Fill::aggressor)
        .def_readwrite("ts_ns",     &Fill::ts_ns);

    py::class_<BookSnapshot>(m, "BookSnapshot")
        .def(py::init<>())
        .def_readwrite("symbol",   &BookSnapshot::symbol)
        .def_readwrite("best_bid", &BookSnapshot::best_bid)
        .def_readwrite("best_ask", &BookSnapshot::best_ask)
        .def_readwrite("bid_sz",   &BookSnapshot::bid_sz)
        .def_readwrite("ask_sz",   &BookSnapshot::ask_sz)
        .def_readwrite("ts_ns",    &BookSnapshot::ts_ns);

    py::class_<DepthLevel>(m, "DepthLevel")
        .def(py::init<>())
        .def_readwrite("price",       &DepthLevel::price)
        .def_readwrite("qty",         &DepthLevel::qty)
        .def_readwrite("order_count", &DepthLevel::order_count);

    // ── LimitOrderBook ────────────────────────────────────────────────────────

    py::class_<LimitOrderBook>(m, "LimitOrderBook")
        .def(py::init<std::string>(), py::arg("symbol"))
        .def("cancel",        &LimitOrderBook::cancel,  py::arg("order_id"))
        .def("best_bid",      [](const LimitOrderBook& b) -> py::object {
            auto v = b.best_bid();
            return v ? py::cast(*v) : py::none();
        })
        .def("best_ask",      [](const LimitOrderBook& b) -> py::object {
            auto v = b.best_ask();
            return v ? py::cast(*v) : py::none();
        })
        .def("best_bid_qty",  [](const LimitOrderBook& b) -> py::object {
            auto v = b.best_bid_qty();
            return v ? py::cast(*v) : py::none();
        })
        .def("best_ask_qty",  [](const LimitOrderBook& b) -> py::object {
            auto v = b.best_ask_qty();
            return v ? py::cast(*v) : py::none();
        })
        .def("spread",        &LimitOrderBook::spread)
        .def("mid",           &LimitOrderBook::mid)
        .def("bid_depth",     &LimitOrderBook::bid_depth, py::arg("n") = 5)
        .def("ask_depth",     &LimitOrderBook::ask_depth, py::arg("n") = 5)
        .def("snapshot",      &LimitOrderBook::snapshot,  py::arg("ts_ns") = 0)
        .def("symbol",        &LimitOrderBook::symbol)
        .def("empty",         &LimitOrderBook::empty);

    // ── MatchResult ───────────────────────────────────────────────────────────

    py::class_<MatchResult>(m, "MatchResult")
        .def_readwrite("fills",         &MatchResult::fills)
        .def_readwrite("accepted",      &MatchResult::accepted)
        .def_readwrite("reject_reason", &MatchResult::reject_reason);

    // ── MatchingEngine ────────────────────────────────────────────────────────

    py::class_<MatchingEngine>(m, "MatchingEngine")
        .def(py::init<>())
        .def("submit",          &MatchingEngine::submit, py::arg("order"))
        // Batch hot path: build + submit N orders entirely in C++ so the
        // pybind11 boundary is crossed once for the whole batch instead of
        // once per order. This is where C++ matching actually beats Python —
        // per-order calls are dominated by marshaling, not matching.
        // sides: 0=Buy 1=Sell ; types: 0=Limit 1=Market 2=IOC 3=FOK
        .def("submit_batch",
            [](MatchingEngine& e, const std::string& symbol,
               const std::vector<int>& sides, const std::vector<int>& types,
               const std::vector<double>& prices,
               const std::vector<uint64_t>& qtys) {
                size_t n = sides.size();
                uint64_t total_fills = 0, total_qty = 0;
                uint64_t base = 1'000'000'000ULL;
                for (size_t i = 0; i < n; ++i) {
                    Order o;
                    o.id     = base + i + 1;
                    o.symbol = symbol;
                    o.side   = sides[i] ? Side::Sell : Side::Buy;
                    switch (types[i]) {
                        case 1:  o.type = OrdType::Market; break;
                        case 2:  o.type = OrdType::IOC;    break;
                        case 3:  o.type = OrdType::FOK;    break;
                        default: o.type = OrdType::Limit;
                    }
                    o.price = prices[i];
                    o.qty   = static_cast<decltype(o.qty)>(qtys[i]);
                    auto res = e.submit(o);
                    total_fills += res.fills.size();
                    for (auto& f : res.fills) total_qty += f.qty;
                }
                py::dict d;
                d["fills"] = total_fills;
                d["qty"]   = total_qty;
                return d;
            },
            py::arg("symbol"), py::arg("sides"), py::arg("types"),
            py::arg("prices"), py::arg("qtys"),
            "Build + submit N orders in C++ (one boundary crossing). "
            "Returns {'fills','qty'}.")
        .def("cancel",          &MatchingEngine::cancel, py::arg("order_id"))
        .def("next_order_id",   &MatchingEngine::next_order_id)
        .def("book",            [](MatchingEngine& e, const std::string& sym) -> py::object {
            auto* b = e.book(sym);
            if (!b) return py::none();
            return py::cast(b, py::return_value_policy::reference);
        }, py::arg("symbol"))
        .def_readwrite("on_fill",        &MatchingEngine::on_fill)
        .def_readwrite("on_add",         &MatchingEngine::on_add)
        .def_readwrite("on_cancel",      &MatchingEngine::on_cancel)
        .def_readwrite("on_book_update", &MatchingEngine::on_book_update);

    // ── Risk ──────────────────────────────────────────────────────────────────

    py::class_<KillSwitch>(m, "KillSwitch")
        .def(py::init<>())
        .def("arm",        &KillSwitch::arm)
        .def("disarm",     &KillSwitch::disarm)
        .def("is_active",  &KillSwitch::is_active);

    py::class_<PreTradeRisk::Limits>(m, "RiskLimits")
        .def(py::init<>())
        .def_readwrite("max_order_qty",         &PreTradeRisk::Limits::max_order_qty)
        .def_readwrite("max_position_notional", &PreTradeRisk::Limits::max_position_notional)
        .def_readwrite("max_price_dev_bps",     &PreTradeRisk::Limits::max_price_dev_bps)
        .def_readwrite("order_rate_per_sec",    &PreTradeRisk::Limits::order_rate_per_sec)
        .def_readwrite("burst",                 &PreTradeRisk::Limits::burst);

    py::enum_<PreTradeRisk::Result>(m, "RiskResult")
        .value("PASS",               PreTradeRisk::Result::PASS)
        .value("REJECT_KILL_SWITCH", PreTradeRisk::Result::REJECT_KILL_SWITCH)
        .value("REJECT_QTY",         PreTradeRisk::Result::REJECT_QTY)
        .value("REJECT_NOTIONAL",    PreTradeRisk::Result::REJECT_NOTIONAL)
        .value("REJECT_RATE",        PreTradeRisk::Result::REJECT_RATE)
        .value("REJECT_PRICE",       PreTradeRisk::Result::REJECT_PRICE)
        .export_values();

    py::class_<PreTradeRisk>(m, "PreTradeRisk")
        .def(py::init<KillSwitch&, PreTradeRisk::Limits>(),
             py::arg("kill_switch"), py::arg("limits") = PreTradeRisk::Limits{})
        .def("check", &PreTradeRisk::check,
             py::arg("order"), py::arg("mid_price"), py::arg("current_pos_notional"))
        .def("limits",     &PreTradeRisk::limits)
        .def("set_limits", &PreTradeRisk::set_limits);

    // ── TradingMetrics ────────────────────────────────────────────────────────

    py::class_<TradingMetrics>(m, "TradingMetrics")
        .def(py::init<>())
        .def("record_order_sent",    &TradingMetrics::record_order_sent)
        .def("record_order_rejected",&TradingMetrics::record_order_rejected)
        .def("record_cancel",        &TradingMetrics::record_cancel)
        .def("record_fill",          &TradingMetrics::record_fill,
             py::arg("side"), py::arg("qty"), py::arg("pnl_cents"))
        .def("record_latency_us",    &TradingMetrics::record_latency_us,
             py::arg("us"))
        .def("percentile",           &TradingMetrics::percentile, py::arg("p"))
        .def("reset",                &TradingMetrics::reset)
        .def_property_readonly("orders_sent",
            [](const TradingMetrics& m){ return m.orders_sent.load(); })
        .def_property_readonly("orders_filled",
            [](const TradingMetrics& m){ return m.orders_filled.load(); })
        .def_property_readonly("orders_cancelled",
            [](const TradingMetrics& m){ return m.orders_cancelled.load(); })
        .def_property_readonly("orders_rejected",
            [](const TradingMetrics& m){ return m.orders_rejected.load(); })
        .def_property_readonly("net_position",
            [](const TradingMetrics& m){ return m.net_position.load(); })
        .def_property_readonly("gross_pnl_cents",
            [](const TradingMetrics& m){ return m.gross_pnl_cents.load(); });

    // ── Feed ──────────────────────────────────────────────────────────────────

    py::class_<TickEvent>(m, "TickEvent")
        .def(py::init<>())
        .def_readwrite("ts_ns",    &TickEvent::ts_ns)
        .def_readwrite("symbol",   &TickEvent::symbol)
        .def_readwrite("price",    &TickEvent::price)
        .def_readwrite("bid",      &TickEvent::bid)
        .def_readwrite("ask",      &TickEvent::ask)
        .def_readwrite("size",     &TickEvent::size)
        .def_readwrite("is_trade", &TickEvent::is_trade);

    py::class_<CsvTickReplay>(m, "CsvTickReplay")
        .def(py::init<>())
        .def("load",       &CsvTickReplay::load,  py::arg("path"))
        .def("play",       &CsvTickReplay::play,  py::arg("callback"),
             py::arg("speed_multiplier") = 0.0)
        .def("tick_count", &CsvTickReplay::tick_count)
        .def("ticks",      &CsvTickReplay::ticks);

    py::class_<SyntheticFeed::Params>(m, "SyntheticParams")
        .def(py::init<>())
        .def_readwrite("symbol",        &SyntheticFeed::Params::symbol)
        .def_readwrite("initial_price", &SyntheticFeed::Params::initial_price)
        .def_readwrite("mu",            &SyntheticFeed::Params::mu)
        .def_readwrite("sigma",         &SyntheticFeed::Params::sigma)
        .def_readwrite("tick_size",     &SyntheticFeed::Params::tick_size)
        .def_readwrite("spread_ticks",  &SyntheticFeed::Params::spread_ticks)
        .def_readwrite("seed",          &SyntheticFeed::Params::seed);

    py::class_<SyntheticFeed>(m, "SyntheticFeed")
        .def(py::init<SyntheticFeed::Params>(),
             py::arg("params") = SyntheticFeed::Params{})
        .def("next_tick", &SyntheticFeed::next_tick)
        .def("generate",  &SyntheticFeed::generate, py::arg("n"));
}
