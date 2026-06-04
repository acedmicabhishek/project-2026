// Minimal self-contained test runner (no external framework required)
// Build: cmake --build build && ./build/test_orderbook

#include "hft_simulator/order.hpp"
#include "hft_simulator/orderbook.hpp"

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

static uint64_t id_seq = 0;

Order make_order(Side side, double price, uint32_t qty,
                  OrdType type = OrdType::Limit,
                  std::string sym = "AAPL") {
    Order o;
    o.id     = ++id_seq;
    o.symbol = sym;
    o.side   = side;
    o.type   = type;
    o.price  = price;
    o.qty    = qty;
    return o;
}

// ─── Tests ───────────────────────────────────────────────────────────────────

void test_empty_book() {
    LimitOrderBook lob("AAPL");
    ASSERT(lob.empty());
    ASSERT(!lob.best_bid().has_value());
    ASSERT(!lob.best_ask().has_value());
    ASSERT_NEAR(lob.spread(), 0.0, 1e-9);
    ASSERT_NEAR(lob.mid(), 0.0, 1e-9);
}

void test_add_bid_ask() {
    LimitOrderBook lob("AAPL");
    auto bid = make_order(Side::Buy,  100.0, 200);
    auto ask = make_order(Side::Sell, 100.5, 150);
    lob.add(bid);
    lob.add(ask);

    ASSERT_NEAR(*lob.best_bid(), 100.0, 1e-9);
    ASSERT_NEAR(*lob.best_ask(), 100.5, 1e-9);
    ASSERT_EQ(*lob.best_bid_qty(), 200u);
    ASSERT_EQ(*lob.best_ask_qty(), 150u);
    ASSERT_NEAR(lob.spread(), 0.5, 1e-6);
    ASSERT_NEAR(lob.mid(), 100.25, 1e-6);
    ASSERT(!lob.empty());
}

void test_multiple_bid_levels() {
    LimitOrderBook lob("AAPL");
    auto b1 = make_order(Side::Buy, 100.0, 100);
    auto b2 = make_order(Side::Buy,  99.5, 200);
    auto b3 = make_order(Side::Buy,  99.0, 300);
    lob.add(b1); lob.add(b2); lob.add(b3);

    // Best bid should be highest price
    ASSERT_NEAR(*lob.best_bid(), 100.0, 1e-9);

    auto depth = lob.bid_depth(3);
    ASSERT_EQ(static_cast<int>(depth.size()), 3);
    ASSERT_NEAR(depth[0].price, 100.0, 1e-9);
    ASSERT_NEAR(depth[1].price,  99.5, 1e-9);
    ASSERT_NEAR(depth[2].price,  99.0, 1e-9);
}

void test_cancel_order() {
    LimitOrderBook lob("AAPL");
    auto bid = make_order(Side::Buy, 100.0, 500);
    lob.add(bid);
    ASSERT_EQ(*lob.best_bid_qty(), 500u);

    bool cancelled = lob.cancel(bid.id);
    ASSERT(cancelled);
    ASSERT(lob.empty());
    ASSERT(bid.status == OrdStatus::Cancelled);

    // Double-cancel returns false
    ASSERT(!lob.cancel(bid.id));
}

void test_cancel_one_of_two_at_same_price() {
    LimitOrderBook lob("AAPL");
    auto b1 = make_order(Side::Buy, 100.0, 300);
    auto b2 = make_order(Side::Buy, 100.0, 200);
    lob.add(b1); lob.add(b2);
    ASSERT_EQ(*lob.best_bid_qty(), 500u);

    lob.cancel(b1.id);
    ASSERT_EQ(*lob.best_bid_qty(), 200u);
    ASSERT_NEAR(*lob.best_bid(), 100.0, 1e-9);
}

void test_modify_decrease_qty() {
    LimitOrderBook lob("AAPL");
    auto b = make_order(Side::Buy, 100.0, 500);
    lob.add(b);
    lob.modify(b.id, 300);
    ASSERT_EQ(*lob.best_bid_qty(), 300u);
}

void test_depth_correct_order() {
    LimitOrderBook lob("AAPL");
    // Asks should be sorted lowest first
    auto a1 = make_order(Side::Sell, 101.0, 100);
    auto a2 = make_order(Side::Sell, 100.5, 200);
    auto a3 = make_order(Side::Sell, 102.0, 50);
    lob.add(a1); lob.add(a2); lob.add(a3);

    auto depth = lob.ask_depth(3);
    ASSERT_NEAR(depth[0].price, 100.5, 1e-9); // lowest ask first
    ASSERT_NEAR(depth[1].price, 101.0, 1e-9);
    ASSERT_NEAR(depth[2].price, 102.0, 1e-9);
}

void test_snapshot() {
    LimitOrderBook lob("TSLA");
    auto bid = make_order(Side::Buy,  199.0, 100);
    auto ask = make_order(Side::Sell, 200.0, 150);
    lob.add(bid); lob.add(ask);

    auto snap = lob.snapshot(12345);
    ASSERT(snap.symbol == "TSLA");
    ASSERT_NEAR(snap.best_bid, 199.0, 1e-9);
    ASSERT_NEAR(snap.best_ask, 200.0, 1e-9);
    ASSERT_EQ(snap.bid_sz, 100u);
    ASSERT_EQ(snap.ask_sz, 150u);
    ASSERT_EQ(snap.ts_ns, 12345u);
}

void test_fifo_within_level() {
    // Two orders at same price: earlier one must appear first in queue
    LimitOrderBook lob("AAPL");
    auto b1 = make_order(Side::Buy, 100.0, 100);
    auto b2 = make_order(Side::Buy, 100.0, 200);
    lob.add(b1); lob.add(b2);

    auto* level = lob.best_bid_level();
    ASSERT(level != nullptr);
    ASSERT_EQ(level->orders.size(), 2u);
    ASSERT_EQ(level->orders.front()->id, b1.id); // b1 was added first
}

} // anonymous namespace

int main() {
    std::cout << "\nLimitOrderBook Tests\n"
              << std::string(40, '=') << "\n";

    run_test("Empty book",              test_empty_book);
    run_test("Add bid and ask",         test_add_bid_ask);
    run_test("Multiple bid levels",     test_multiple_bid_levels);
    run_test("Cancel order",            test_cancel_order);
    run_test("Cancel one of two",       test_cancel_one_of_two_at_same_price);
    run_test("Modify decrease qty",     test_modify_decrease_qty);
    run_test("Ask depth order",         test_depth_correct_order);
    run_test("Snapshot",                test_snapshot);
    run_test("FIFO within level",       test_fifo_within_level);

    std::cout << std::string(40, '=') << "\n";
    std::cout << "Results: " << (tests_run - tests_failed)
              << "/" << tests_run << " passed";
    if (tests_failed > 0)
        std::cout << "  (" << tests_failed << " FAILED)";
    std::cout << "\n\n";

    return tests_failed > 0 ? 1 : 0;
}
