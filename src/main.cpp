#include "hft_simulator/market.h"
#include "hft_simulator/engine.h"
#include "hft_simulator/strategy.h"
#include <iostream>
#include <memory>

int main() {
    hft::MarketDataGenerator generator(100.0, 0.002, 42);
    auto market_data = generator.generate_series(500);
    hft::OrderBook book(100.0);
    auto strategy = std::make_unique<hft::MomentumStrategy>(4);
    hft::SimulationEngine engine(std::move(book), std::move(strategy), 0.0);

    hft::SimulationSummary summary = engine.run(market_data);

    std::cout << "=== HFT Simulation Summary ===\n";
    std::cout << "Strategy: " << summary.strategy << "\n";
    std::cout << "Orders executed: " << summary.orders_executed << "\n";
    std::cout << "Trade count: " << summary.trade_count << "\n";
    std::cout << "PnL: " << summary.pnl << "\n";
    std::cout << "Runtime (s): " << summary.runtime_seconds << "\n";
    std::cout << "Average latency (s): " << summary.average_latency_seconds << "\n";

    std::cout << "\nSample trades:\n";
    int printed = 0;
    for (auto const& trade : engine.trades()) {
        if (printed++ >= 5) break;
        std::cout << "  " << (trade.side == hft::Order::Side::Buy ? "Buy" : "Sell")
                  << " @ " << trade.price << " size=" << trade.size << "\n";
    }
    return 0;
}
