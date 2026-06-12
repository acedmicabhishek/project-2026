#include "hft_simulator/sim_core.hpp"
#include <iostream>

int main() {
    hft::SimState state;
    hft::SimRunner runner(state);

    std::cout << "Starting simulation (mm with 5B)..." << std::endl;
    runner.start("mm", 5000, 0.0008, 42, 5000000000.0, true, []() {});

    while (state.running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Fills total (strategy): " << state.fills_total << std::endl;
    std::cout << "Fills total (bots): " << state.bot_fills_total << std::endl;
    std::cout << "Bot volume: " << state.bot_volume << std::endl;

    for (size_t i = 0; i < state.bot_equity.size(); ++i) {
        if (i % 500 == 0 || i == state.bot_equity.size() - 1) {
            std::cout << "Tick " << i << ": Strategy Equity = " << state.equity[i] 
                      << ", Bot Equity = " << state.bot_equity[i] << std::endl;
        }
    }
    return 0;
}
