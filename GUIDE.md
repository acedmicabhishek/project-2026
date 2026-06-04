# QuantSim — Feature Guide & Testing Walkthrough

## Table of Contents
1. [Quick Build](#1-quick-build)
2. [C++ Unit Tests](#2-c-unit-tests)
3. [GUI — Launch & Layout](#3-gui--launch--layout)
4. [Simulation Tab — Running Strategies](#4-simulation-tab--running-strategies)
5. [Dashboard Tab — Live Market View](#5-dashboard-tab--live-market-view)
6. [Equity Tab — Backtest Results](#6-equity-tab--backtest-results)
7. [Results Tab — Metrics Report](#7-results-tab--metrics-report)
8. [Editor Tab — Write & Run Strategies](#8-editor-tab--write--run-strategies)
9. [Python CLI — Backtesting & Analytics](#9-python-cli--backtesting--analytics)
10. [Strategy Examples](#10-strategy-examples)
11. [Data Feeds](#11-data-feeds)
12. [Risk Controls](#12-risk-controls)

---

## 1. Quick Build

### Prerequisites
| Tool | Version | Install |
|------|---------|---------|
| CMake | ≥ 3.23 | `brew install cmake` |
| Clang/GCC | C++23 support | Xcode CLT or `brew install llvm` |
| gtkmm | 4.x | `brew install gtkmm4` |
| Python | 3.10+ | `brew install python` |
| pybind11 | any | `pip install pybind11` |
| reportlab | any | `pip install reportlab` (for synopsis PDF only) |

### Build
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Verify build outputs
```
build/hft_gui           ← GTK4 desktop app
build/run_simulation    ← C++ CLI entry
build/test_orderbook    ← LOB unit tests
build/test_matching     ← Matching engine unit tests
```

---

## 2. C++ Unit Tests

Run all 21 tests:
```bash
cd build && ctest --output-on-failure
```

Run individual test binaries for verbose output:
```bash
./build/test_orderbook
./build/test_matching
```

### What is tested

**Order Book (`test_orderbook`):**
- Empty book returns no best bid/ask
- Add bid → best bid updates correctly
- Add ask → best ask updates correctly
- Cancel → removes order, level disappears if empty
- Cancel one of two orders at same price level
- Modify quantity in-place
- Depth order (bids descending, asks ascending)
- Snapshot returns correct side/price/size
- FIFO priority within same price level

**Matching Engine (`test_matching`):**
- No match when spread exists
- Market order sweeps full fill
- Limit order full fill on crossing price
- Limit order partial fill, remainder rests in book
- IOC: partial fill, remainder cancelled
- FOK: reject if full fill not possible (book untouched)
- FOK: accept if full fill is possible
- FIFO order preserved across multiple orders at same level
- Cancel removes resting order
- Multi-level sweep: market order crosses multiple price levels
- Fill callback fires with correct price/qty
- `on_book_update` fires after every submission

---

## 3. GUI — Launch & Layout

```bash
./build/hft_gui
```

### Window layout
```
┌─ QuantSim ─────────────────────────────────────────────────────────┐
│  [Sidebar: params]  │  [Tabs: Dashboard | Equity | Results | Editor]│
│                     │                                               │
│  Strategy: [▼]      │  Tab content changes based on selection       │
│  Ticks:    [▲▼]     │                                               │
│  Sigma:    [▲▼]     │                                               │
│  Seed:     [▲▼]     │                                               │
│  Cash:     [▲▼]     │                                               │
│  Speed:    [────]   │                                               │
│  [▶ Run]  [■ Stop]  │                                               │
│                     │                                               │
│  ─── Metrics ───    │                                               │
│  Sharpe / Sortino   │                                               │
│  Max DD  / Vol      │                                               │
│  Fills  / Orders    │                                               │
│  PnL    / Ticks     │                                               │
│                     │                                               │
│  ─── Log ───        │                                               │
│  [live messages]    │                                               │
│                     │                                               │
│  Kill Switch: ✓ OK  │  [☀ theme toggle top-right]                  │
└─────────────────────┴───────────────────────────────────────────────┘
```

### Theme toggle
Click `☀` (top-right) to switch dark ↔ light. Cairo charts and all CSS panels update instantly.

---

## 4. Simulation Tab — Running Strategies

### Controls

| Control | Range | Effect |
|---------|-------|--------|
| **Strategy** | Market Making / Stat Arb / TWAP | Selects algorithm |
| **Ticks** | 1000 – 100000 | Number of synthetic market ticks to simulate |
| **Sigma** | 0.0001 – 0.05 | GBM/OU volatility per tick |
| **Seed** | 1 – 99999 | RNG seed for reproducibility (same seed → same results) |
| **Cash** | $10k – $10M | Initial portfolio cash |
| **Speed** | 1× – 100× | Simulation replay speed multiplier |

### Test each strategy

#### Market Making (Avellaneda-Stoikov)
```
Strategy: Market Making
Ticks:    10000
Sigma:    0.0010
Seed:     42
Cash:     500000
```
Expected: positive or near-zero PnL, high fill count, tight equity curve oscillation.

#### Statistical Arbitrage (OU Spread)
```
Strategy: Stat Arb
Ticks:    5000
Sigma:    0.0008
Seed:     7
Cash:     500000
```
Expected: discrete step changes in equity when spread crosses ±2σ z-score threshold. Position alternates long/short.

#### TWAP Execution (Almgren-Chriss IS)
```
Strategy: TWAP
Ticks:    10000
Sigma:    0.0005
Seed:     1
Cash:     500000
```
Expected: equity decreases monotonically (buying 5000 shares in 20 slices at ask). Fill count = 20 slices.

### Stop mid-run
Press `■ Stop` at any tick. Metrics compute on partial data. Can restart with different params.

### Reproducibility test
Run same strategy twice with identical seed → equity curve must be pixel-perfect identical.
Change seed by 1 → different price path, different PnL.

---

## 5. Dashboard Tab — Live Market View

While simulation is running, switch to **📊 Dashboard** tab.

### Left panel: Order Book Depth chart
- Horizontal bar chart, top 8 bid/ask levels
- Bids: green bars (left/right from center)
- Asks: red bars
- Updates every refresh tick (~100ms)
- Best bid/ask prices labeled

### Right panel: Fill Log
Columns: `ID | Symbol | Side | Qty | Price`

- Updates live as fills arrive
- BUY rows: green text
- SELL rows: red text
- Scrollable; last 200 fills retained

**Test:** Run Market Making with 5000 ticks, watch bid/ask depth shift as inventory builds.

---

## 6. Equity Tab — Backtest Results

Switch to **📈 Equity** tab after or during run.

### Equity curve chart
- X-axis: tick number
- Y-axis: portfolio value ($)
- Green line = equity above initial cash
- Red line = equity below initial cash
- Horizontal dashed reference = initial cash level
- Auto-scales to min/max of equity history

**Test:** Run TWAP → equity should slope down (cash spent buying). Run Market Making → equity oscillates around starting value.

---

## 7. Results Tab — Metrics Report

Switch to **📋 Results** tab after simulation completes.

Shows formatted text report:
```
Strategy:     Market Making
Ticks run:    10000
Final PnL:    $+1,234.56
Sharpe:       1.82
Sortino:      2.41
Calmar:       0.94
Max Drawdown: 3.21%
Volatility:   12.4% ann.
Total Fills:  847
Orders Sent:  1694
Net Position: +100
```

**Sidebar metrics** update live while running:
- **Sharpe** — annualized mean return / volatility
- **Sortino** — annualized mean return / downside deviation
- **Max DD** — peak-to-trough drawdown %
- **Vol** — annualized volatility %

---

## 8. Editor Tab — Write & Run Strategies

Click **✏️ Editor** tab.

### Layout
```
┌──────────────┬──────────────────────────────────────────┐
│ Strategy     │ [filename.py ●]  [💾 Save] [▶ Run]       │
│ Files        │ [■ Stop] [⎋ Open in Editor]              │
│              ├──────────────────────────────────────────┤
│ market_      │  """                                      │
│ maker.py     │  QuantSim Strategy                        │
│ stat_arb.py  │  """                                      │
│ twap.py      │  import sys                               │
│ [mine]       │  from python.quantsim.strategy import ... │
│ strategy_1   │                                           │
│ .py          │  class MyStrategy(Strategy):              │
│              │      def on_book_update(self, u):         │
│ [+ New]      │          ...                              │
│ [Open…]      ├──────────────────────────────────────────┤
│              │ Console Output                  [Clear]   │
│              │ ▶  python3 strategy_1.py                  │
│              │   mid=100.0241  pos=0  pnl=0.00           │
│              │   Fill: B 100 @ 100.0100                  │
│              │ ──── exit 0 ────                           │
└──────────────┴──────────────────────────────────────────┘
```

### Workflow: create and run a strategy

1. Click **`+ New`** — creates `~/.quantsim/strategies/strategy_N.py` with a working template
2. Edit the code in the center panel
3. `●` indicator appears in filename when unsaved
4. Click **`💾 Save`** or the file saves automatically before Run
5. Click **`▶ Run`** — executes `python3 strategy_N.py`, streams output to console
6. Click **`■ Stop`** to abort a long-running backtest
7. Click **`⎋ Open in Editor`** to open the file in VSCode (falls back to system default)

### Load an existing example
Click `market_maker.py`, `stat_arb.py`, or `twap.py` in the file list → loads into editor.

### Test: verify run works
1. Click `+ New`
2. Press `▶ Run` immediately (template runs out of the box)
3. Console shows tick output + `exit 0`

---

## 9. Python CLI — Backtesting & Analytics

From the project root:

### Run example strategies directly
```bash
python3 examples/market_maker.py --ticks 5000 --sigma 0.001 --seed 42
python3 examples/stat_arb.py     --ticks 3000 --seed 7
python3 examples/twap.py         --ticks 8000 --qty 5000
```

### Run the interactive terminal dashboard
```bash
python3 run_sim.py
```
Launches a `rich`-powered live terminal dashboard with:
- Order book depth table (top 5 levels)
- Recent fills log
- Position / PnL / Sharpe live update
- Kill switch status

### Backtest in Python REPL
```python
from python.quantsim.backtester import BacktestEngine
from python.quantsim.data.synthetic import GBMFeed
from python.quantsim.analytics import compute_metrics

feed   = GBMFeed("AAPL", initial_price=150.0, sigma=0.001, n_ticks=10000)
engine = BacktestEngine()

# Use built-in Market Making strategy
from examples.market_maker import AvellanedaStoikov
result = engine.run(feed, AvellanedaStoikov, initial_cash=500_000.0)

metrics = compute_metrics(result.equity_curve, result.fills)
print(metrics)  # {'sharpe': ..., 'sortino': ..., 'max_drawdown': ..., ...}
```

### Walk-forward validation
```python
from python.quantsim.backtester import WalkForwardTest
wf = WalkForwardTest(train_window=2000, test_window=500)
wf.run(AvellanedaStoikov, feed)
```

### Purged K-Fold cross-validation
```python
from python.quantsim.backtester import PurgedKFold
pkf = PurgedKFold(n_splits=5, embargo=0.01)
for train_idx, test_idx in pkf.split(X, pred_times, eval_times):
    ...
```

---

## 10. Strategy Examples

### Market Making (`examples/market_maker.py`)
Implements **Avellaneda-Stoikov** optimal spread:

```
reservation price:  r  = mid − q · γ · σ² · T
optimal spread:     δ* = γ · σ² · T + (2/γ) · ln(1 + γ/κ)
```

- `γ` = risk aversion (default 0.1)
- `κ` = order arrival rate (default 1.5)
- Reprices on mid move > 0.5 bps
- Inventory limit: ±1000 shares → quotes pause

### Statistical Arbitrage (`examples/stat_arb.py`)
OU spread between SPY/QQQ:

```
spread = price_A − price_B
z-score = (spread − rolling_mean) / rolling_std

Entry:  |z| > 2.0
Exit:   |z| < 0.3
```

### TWAP + Implementation Shortfall (`examples/twap.py`)
Almgren-Chriss optimal trajectory:

```
q(t) = Q · sinh(κ(T−t)) / sinh(κT)
κ     = √(λσ² / η)
```

Liquidates `Q` shares in `N` equal time slices, reports IS cost vs VWAP benchmark.

---

## 11. Data Feeds

### Synthetic GBM (built-in, always available)
```python
from python.quantsim.data.synthetic import GBMFeed
feed = GBMFeed("AAPL", initial_price=150.0, sigma=0.001, n_ticks=10000, seed=42)
```
Uses Euler-Maruyama: `S(t+1) = S(t) · exp((μ − σ²/2) + σ · dW)`

### OU Spread Feed (two correlated assets)
```python
from python.quantsim.data.synthetic import OUSpreadFeed
feed = OUSpreadFeed("SPY", "QQQ", initial_price=450.0,
                    theta=0.08, mu_spread=0.0, sigma_spread=0.05)
```

### CSV Replay
```bash
# CSV format: ts_ns,symbol,price,bid,ask,size,is_trade
python3 -c "
from python.quantsim.data.replay import TickReplay
replay = TickReplay()
replay.load('data/my_ticks.csv')
replay.play(speed=10.0)   # 10× real-time
"
```

### Nasdaq ITCH 5.0 Binary (C++)
```cpp
ITCHParser parser;
parser.on_add_order = [](const ITCHAddOrder& msg, uint64_t ts) { ... };
parser.on_trade     = [](const ITCHTrade& msg,    uint64_t ts) { ... };
parser.parse_file("20240101.NASDAQ_ITCH50");
```
Download sample files from Nasdaq's FTP (link in ITCH 5.0 spec).

---

## 12. Risk Controls

### Kill Switch (4-level hierarchy)

| Level | Latency | Trigger |
|-------|---------|---------|
| Strategy software kill | < 1 ms | `ks.arm()` in strategy code |
| Portfolio risk engine kill | < 5 ms | Pre-trade limit breach |
| Gateway connection drop | < 10 ms | Manual / programmatic |
| Exchange Mass Cancel | 1–50 ms | On disconnect |

```python
# In a strategy:
if abs(self.position) > 2000:
    self.gateway.kill_switch.arm()   # halts all order submission
```

### Pre-Trade Risk Limits (defaults)

| Check | Limit |
|-------|-------|
| Max order qty | 10,000 shares |
| Max position notional | $1,000,000 |
| Price sanity (from mid) | ±50 bps |
| Rate limiter | 500 orders / second |
| Stale price guard | 5 ms feed gap → arms kill switch |

### Test kill switch via GUI
1. Run any strategy
2. Watch **Kill Switch** indicator at bottom of sidebar
3. Green `✓ OK` = normal
4. Arm condition fires when position limit hits → turns `⚠ ARMED` (red)

---

## Quick Smoke Test Checklist

```
[ ] cmake --build build && ctest passes (21/21)
[ ] ./build/hft_gui launches, no crash
[ ] Dark/light theme toggle works (☀ button)
[ ] Market Making sim completes, equity curve draws
[ ] Stat Arb sim completes, step-change equity pattern visible
[ ] TWAP sim completes, fill count = 20
[ ] Dashboard depth chart updates live during run
[ ] Fill log populates with BUY/SELL rows
[ ] Editor tab loads, file browser shows examples/
[ ] + New creates strategy file with template
[ ] ▶ Run in editor executes python3 and streams output
[ ] ⎋ Open in Editor launches VSCode (or system default)
[ ] python3 examples/market_maker.py --ticks 1000 prints results
[ ] python3 run_sim.py shows rich terminal dashboard
```

---

*QuantSim — Cambridge Institute of Technology, Final Year Project 2025–2026*
*Abhishek Anand (360/CSE/22) · Shakir Ahmad (323/CSE/22) · Amik Affan (371/CSE/22)*
