// Matching engine tests
// Build: cmake --build build && ./build/test_matching

#include "hft_simulator/order.hpp"
#include "hft_simulator/orderbook.hpp"
#include "hft_simulator/matching.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <functional>

namespace {

int tests_run = 0, tests_failed = 0;

void run_test(const std::string& name, std::function<void()> fn) {
    ++tests_run;
    try {
        fn();
        std::cout << "  [PASS] " << name << "\n";
    } catch (const std::exception& e) {
        ++tests_failed;
        std::cout << "  [FAIL] " << name << " — " << e.what() << "\n";
    } catch (...) {
        ++tests_failed;
        std::cout << "  [FAIL] " << name << " — unknown exception\n";
    }
}

#define ASSERT(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond \
                                          " at line " + std::to_string(__LINE__))
#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error( \
        "Expected equal: " + std::to_string(a) + " != " + std::to_string(b) + \
        " at line " + std::to_string(__LINE__))
#define ASSERT_NEAR(a, b, eps) \
    if (std::abs((double)(a) - (double)(b)) > (eps)) throw std::runtime_error( \
        "Not near: " + std::to_string(a) + " vs " + std::to_string(b) + \
        " at line " + std::to_string(__LINE__))

using namespace hft;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static MatchingEngine make_engine() { return MatchingEngine{}; }

Order make_order(MatchingEngine& eng, Side side, double price, uint32_t qty,
                  OrdType type = OrdType::Limit, std::string sym = "AAPL") {
    Order o;
    o.id     = eng.next_order_id();
    o.symbol = sym;
    o.side   = side;
    o.type   = type;
    o.price  = price;
    o.qty    = qty;
    return o;
}

// ─── Tests ───────────────────────────────────────────────────────────────────

void test_limit_order_no_match() {
    auto eng = make_engine();
    auto bid = make_order(eng, Side::Buy, 100.0, 500);
    auto result = eng.submit(bid);
    ASSERT(result.accepted);
    ASSERT(result.fills.empty());

    auto* lob = eng.book("AAPL");
    ASSERT(lob != nullptr);
    ASSERT_NEAR(*lob->best_bid(), 100.0, 1e-9);
}

void test_market_order_fills_limit() {
    auto eng = make_engine();
    // Place a limit ask
    auto ask = make_order(eng, Side::Sell, 100.0, 300);
    eng.submit(ask);

    // Market buy — should fill against the ask
    auto buy = make_order(eng, Side::Buy, 0.0, 200, OrdType::Market);
    auto result = eng.submit(buy);
    ASSERT(result.accepted);
    ASSERT_EQ(static_cast<int>(result.fills.size()), 1);
    ASSERT_EQ(result.fills[0].qty, 200u);
    ASSERT_NEAR(result.fills[0].price, 100.0, 1e-9);
}

void test_full_fill_removes_from_book() {
    auto eng = make_engine();
    auto ask = make_order(eng, Side::Sell, 100.0, 200);
    eng.submit(ask);

    auto buy = make_order(eng, Side::Buy, 0.0, 200, OrdType::Market);
    eng.submit(buy);

    auto* lob = eng.book("AAPL");
    ASSERT(lob->best_ask() == std::nullopt);  // level should be gone
}

void test_partial_fill_updates_book() {
    auto eng = make_engine();
    auto ask = make_order(eng, Side::Sell, 100.0, 500);
    eng.submit(ask);

    auto buy = make_order(eng, Side::Buy, 0.0, 200, OrdType::Market);
    auto result = eng.submit(buy);

    ASSERT_EQ(result.fills[0].qty, 200u);

    auto* lob = eng.book("AAPL");
    ASSERT_EQ(*lob->best_ask_qty(), 300u);  // 500 - 200
}

void test_limit_crossing_fills_immediately() {
    auto eng = make_engine();
    // Passive ask at 100
    auto ask = make_order(eng, Side::Sell, 100.0, 400);
    eng.submit(ask);

    // Aggressive bid at 101 (crosses ask at 100)
    auto bid = make_order(eng, Side::Buy, 101.0, 400, OrdType::Limit);
    auto result = eng.submit(bid);

    ASSERT(result.accepted);
    ASSERT(!result.fills.empty());
    // Fill should happen at maker price (100.0)
    ASSERT_NEAR(result.fills[0].price, 100.0, 1e-9);
}

void test_ioc_partial_fill_remainder_cancelled() {
    auto eng = make_engine();
    // Only 100 available at ask
    auto ask = make_order(eng, Side::Sell, 100.0, 100);
    eng.submit(ask);

    // IOC buy for 300 — fills 100, cancels 200
    auto buy = make_order(eng, Side::Buy, 0.0, 300, OrdType::IOC);
    auto result = eng.submit(buy);

    ASSERT(result.accepted);
    ASSERT_EQ(result.fills[0].qty, 100u);

    // IOC remainder should not be in book
    auto* lob = eng.book("AAPL");
    ASSERT(!lob->best_bid().has_value());
}

void test_fok_full_fill() {
    auto eng = make_engine();
    auto ask = make_order(eng, Side::Sell, 100.0, 500);
    eng.submit(ask);

    auto buy = make_order(eng, Side::Buy, 100.0, 400, OrdType::FOK);
    auto result = eng.submit(buy);
    ASSERT(result.accepted);
    ASSERT_EQ(result.fills[0].qty, 400u);
}

void test_fok_reject_insufficient_liquidity() {
    auto eng = make_engine();
    auto ask = make_order(eng, Side::Sell, 100.0, 100);
    eng.submit(ask);

    // FOK for 500, only 100 available — should reject
    auto buy = make_order(eng, Side::Buy, 100.0, 500, OrdType::FOK);
    auto result = eng.submit(buy);
    ASSERT(!result.accepted);
    ASSERT(!result.reject_reason.empty());
    // Book should still have the original ask untouched
    auto* lob = eng.book("AAPL");
    ASSERT_EQ(*lob->best_ask_qty(), 100u);
}

void test_fifo_matching_order() {
    auto eng = make_engine();
    // Two passive asks at same price — first one should fill first
    auto a1 = make_order(eng, Side::Sell, 100.0, 100);
    auto a2 = make_order(eng, Side::Sell, 100.0, 200);
    eng.submit(a1);
    eng.submit(a2);

    auto buy = make_order(eng, Side::Buy, 0.0, 100, OrdType::Market);
    auto result = eng.submit(buy);

    ASSERT_EQ(result.fills[0].maker_id, a1.id);
}

void test_cancel_live_order() {
    auto eng = make_engine();
    auto bid = make_order(eng, Side::Buy, 99.0, 300);
    eng.submit(bid);

    bool ok = eng.cancel(bid.id);
    ASSERT(ok);

    auto* lob = eng.book("AAPL");
    ASSERT(!lob->best_bid().has_value());
}

void test_multi_level_sweep() {
    auto eng = make_engine();
    // Three ask levels
    auto a1 = make_order(eng, Side::Sell, 100.0, 100);
    auto a2 = make_order(eng, Side::Sell, 101.0, 100);
    auto a3 = make_order(eng, Side::Sell, 102.0, 100);
    eng.submit(a1); eng.submit(a2); eng.submit(a3);

    // Market buy for 250 — sweeps 100@100, 100@101, 50@102
    auto buy = make_order(eng, Side::Buy, 0.0, 250, OrdType::Market);
    auto result = eng.submit(buy);

    ASSERT_EQ(static_cast<int>(result.fills.size()), 3);
    ASSERT_EQ(result.fills[0].qty, 100u);
    ASSERT_NEAR(result.fills[0].price, 100.0, 1e-9);
    ASSERT_EQ(result.fills[1].qty, 100u);
    ASSERT_NEAR(result.fills[1].price, 101.0, 1e-9);
    ASSERT_EQ(result.fills[2].qty, 50u);
    ASSERT_NEAR(result.fills[2].price, 102.0, 1e-9);

    // 50 should remain at 102
    ASSERT_EQ(*eng.book("AAPL")->best_ask_qty(), 50u);
}

void test_fill_callback_fires() {
    auto eng = make_engine();
    int fill_count = 0;
    eng.on_fill = [&](const Fill&) { ++fill_count; };

    auto ask = make_order(eng, Side::Sell, 100.0, 200);
    eng.submit(ask);
    auto buy = make_order(eng, Side::Buy, 0.0, 200, OrdType::Market);
    eng.submit(buy);

    ASSERT_EQ(fill_count, 1);
}

// ─── Order-type tests (post-only, stop, stop-limit, iceberg) ─────────────────

void test_post_only_rejected_on_cross() {
    auto eng = make_engine();
    eng.submit(make_order(eng, Side::Sell, 100.0, 100));       // ask 100
    Order po = make_order(eng, Side::Buy, 100.0, 100, OrdType::PostOnly); // would cross
    auto r = eng.submit(po);
    ASSERT(!r.accepted);
    auto* lob = eng.book("AAPL");
    ASSERT(!lob->best_bid().has_value());                      // nothing rested
}

void test_post_only_rests_when_passive() {
    auto eng = make_engine();
    eng.submit(make_order(eng, Side::Sell, 101.0, 100));       // ask 101
    Order po = make_order(eng, Side::Buy, 100.0, 100, OrdType::PostOnly); // passive
    auto r = eng.submit(po);
    ASSERT(r.accepted);
    auto* lob = eng.book("AAPL");
    ASSERT(lob->best_bid().has_value());
    ASSERT_NEAR(*lob->best_bid(), 100.0, 1e-9);
}

void test_stop_market_triggers_immediately() {
    auto eng = make_engine();
    int fills = 0; eng.on_fill = [&](const Fill&){ ++fills; };
    eng.submit(make_order(eng, Side::Sell, 105.0, 100));       // ask 105
    Order stop = make_order(eng, Side::Buy, 0.0, 100, OrdType::Stop);
    stop.stop_price = 104.0;                                   // ask 105 >= 104 -> fire
    eng.submit(stop);
    ASSERT_EQ(fills, 1);
}

void test_stop_waits_then_triggers() {
    auto eng = make_engine();
    int fills = 0; eng.on_fill = [&](const Fill&){ ++fills; };
    eng.submit(make_order(eng, Side::Sell, 100.0, 100));       // ask 100
    Order stop = make_order(eng, Side::Buy, 0.0, 50, OrdType::Stop);
    stop.stop_price = 105.0;                                   // needs ask >= 105
    eng.submit(stop);
    ASSERT_EQ(fills, 0);                                       // not yet
    eng.submit(make_order(eng, Side::Buy, 100.0, 100));        // take ask 100 (fills=1)
    eng.submit(make_order(eng, Side::Sell, 106.0, 100));       // ask 106 -> stop fires
    ASSERT(fills >= 2);
}

void test_stop_limit_triggers_as_limit() {
    auto eng = make_engine();
    eng.submit(make_order(eng, Side::Sell, 105.0, 100));       // ask 105
    Order sl = make_order(eng, Side::Buy, 105.0, 100, OrdType::StopLimit);
    sl.stop_price = 104.0;                                     // fires, becomes limit @105
    auto r = eng.submit(sl);
    ASSERT(r.accepted);
}

void test_iceberg_hides_and_replenishes() {
    auto eng = make_engine();
    Order ice = make_order(eng, Side::Sell, 100.0, 300, OrdType::Iceberg);
    ice.display_qty = 100;
    eng.submit(ice);
    auto* lob = eng.book("AAPL");
    ASSERT_EQ(lob->qty_at(100.0), 100u);                       // only the clip shows
    eng.submit(make_order(eng, Side::Buy, 0.0, 100, OrdType::Market));
    ASSERT_EQ(lob->qty_at(100.0), 100u);                       // replenished (clip 2)
    eng.submit(make_order(eng, Side::Buy, 0.0, 100, OrdType::Market));
    ASSERT_EQ(lob->qty_at(100.0), 100u);                       // replenished (clip 3)
    eng.submit(make_order(eng, Side::Buy, 0.0, 100, OrdType::Market));
    ASSERT_EQ(lob->qty_at(100.0), 0u);                         // reserve exhausted
}

} // anonymous namespace

int main() {
    std::cout << "\nMatchingEngine Tests\n"
              << std::string(40, '=') << "\n";

    run_test("Limit order no match",           test_limit_order_no_match);
    run_test("Market order fills limit",       test_market_order_fills_limit);
    run_test("Full fill removes from book",    test_full_fill_removes_from_book);
    run_test("Partial fill updates book",      test_partial_fill_updates_book);
    run_test("Limit crossing fills immediately",test_limit_crossing_fills_immediately);
    run_test("IOC partial → remainder cancel", test_ioc_partial_fill_remainder_cancelled);
    run_test("FOK full fill",                  test_fok_full_fill);
    run_test("FOK reject insufficient",        test_fok_reject_insufficient_liquidity);
    run_test("FIFO matching order",            test_fifo_matching_order);
    run_test("Cancel live order",              test_cancel_live_order);
    run_test("Multi-level sweep",              test_multi_level_sweep);
    run_test("Fill callback fires",            test_fill_callback_fires);
    run_test("Post-only rejected on cross",    test_post_only_rejected_on_cross);
    run_test("Post-only rests when passive",   test_post_only_rests_when_passive);
    run_test("Stop market triggers now",       test_stop_market_triggers_immediately);
    run_test("Stop waits then triggers",       test_stop_waits_then_triggers);
    run_test("Stop-limit triggers as limit",   test_stop_limit_triggers_as_limit);
    run_test("Iceberg hides + replenishes",    test_iceberg_hides_and_replenishes);

    std::cout << std::string(40, '=') << "\n";
    std::cout << "Results: " << (tests_run - tests_failed)
              << "/" << tests_run << " passed";
    if (tests_failed > 0)
        std::cout << "  (" << tests_failed << " FAILED)";
    std::cout << "\n\n";

    return tests_failed > 0 ? 1 : 0;
}
