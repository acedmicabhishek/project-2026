# HFT Simulator Project Plan (C++)

## Project Goal
Build a High-Frequency Trading (HFT) simulation platform in C++ that lets a developer:
- write and modify trading logic,
- run fast market simulation,
- measure execution speed and latency,
- compare strategies and order execution behavior.

## Key Features
1. Market simulator with synthetic tick generation and order book snapshots.
2. Strategy interface with sample momentum and mean-reversion strategies.
3. Simulation engine that executes orders, records trades, and reports latency.
4. A runnable C++ example in `src/main.cpp`.

## Architecture
- `include/hft_simulator/market.h` / `src/market.cpp`: market data generation and order book simulation.
- `include/hft_simulator/strategy.h` / `src/strategy.cpp`: strategy base class and strategy examples.
- `include/hft_simulator/engine.h` / `src/engine.cpp`: core simulation engine and trade execution.
- `include/hft_simulator/benchmark.h` / `src/benchmark.cpp`: timing helper utilities.
- `include/hft_simulator/ui.h` / `src/ui.cpp`: GTK UI with a built-in text editor and simulation control panel.
- `src/main.cpp`: command-line simulation runner.
- `src/gui_main.cpp`: GTK GUI entrypoint.
- `CMakeLists.txt`: build configuration.

## Build and Run
1. Create a build directory:
   ```bash
   mkdir -p build && cd build
   ```
2. Configure with CMake (requires CMake 3.23+):
   ```bash
   cmake ..
   ```
3. Build the simulator:
   ```bash
   cmake --build .
   ```
4. Run the command-line example:
   ```bash
   ./run_simulation
   ```
5. If gtkmm-4.0 and pkg-config are installed, the GUI binary will also be available:
   ```bash
   ./hft_gui
   ```

> On macOS, install required GUI dependencies with Homebrew if needed:
> ```bash
> brew install pkg-config gtk4 gtkmm4
> ```

## Next Steps
- add a command-line mode for selecting strategies,
- add CSV output for latency and PnL comparison,
- model fees, slippage, and exchange latency,
- add multi-agent or market maker behavior,
- provide a live visualization or replay mode.

---

## Folder Layout
- `README.md`
- `CMakeLists.txt`
- `include/hft_simulator/`
- `include/hft_simulator/`
  - `benchmark.h`
  - `engine.h`
  - `market.h`
  - `strategy.h`
- `src/`
  - `benchmark.cpp`
  - `engine.cpp`
  - `market.cpp`
  - `strategy.cpp`
  - `ui.cpp`
  - `main.cpp`
  - `gui_main.cpp`

