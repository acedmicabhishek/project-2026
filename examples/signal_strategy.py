"""
EMA Crossover signal strategy — Stage 2 showcase.

Uses quantsim.research.signals.EMACross to generate long/flat signals.
Produces a tear-sheet PNG and trades.csv on completion.

Usage:
    python examples/signal_strategy.py                              # synthetic GBM
    python examples/signal_strategy.py --source yfinance --symbol AAPL
    python examples/signal_strategy.py --source yfinance --symbol NVDA --fast 10 --slow 50
    python examples/signal_strategy.py --no-benchmark              # skip SPY fetch
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))

from quantsim.strategy            import Strategy, BookUpdate, FillEvent
from quantsim.backtester          import BacktestEngine
from quantsim.research.signals    import EMACross
from quantsim.analytics           import compute_metrics


class EMACrossStrategy(Strategy):
    """
    Long / flat EMA crossover.

    - fast EMA > slow EMA → go long (market buy)
    - fast EMA < slow EMA → exit long (market sell)
    - Never goes short.
    """

    def __init__(self, symbol: str, fast: int = 10, slow: int = 30,
                 order_qty: int = 100):
        super().__init__()
        self.symbol    = symbol
        self.order_qty = order_qty
        self._signal   = EMACross(fast, slow)
        self._position = 0
        self._prev_sig = 0

    def on_start(self):
        print(f"[EMA] Starting EMACross on {self.symbol} "
              f"(fast={self._signal._fast.period}, slow={self._signal._slow.period})")

    def on_book_update(self, update: BookUpdate):
        if update.symbol != self.symbol:
            return

        sig = self._signal.update(update.mid)

        if not self._signal.ready or sig == self._prev_sig:
            return

        if sig == 1 and self._position == 0:
            # Cross up → enter long
            self.send_order(self.symbol, 'B', self.order_qty, update.ask, 'MARKET')

        elif sig == -1 and self._position > 0:
            # Cross down → exit long
            self.send_order(self.symbol, 'S', self._position, update.bid, 'MARKET')

        self._prev_sig = sig

    def on_fill(self, fill: FillEvent):
        super().on_fill(fill)
        delta = fill.qty if fill.side in ('B', 'BUY') else -fill.qty
        self._position += delta


# ── Walk-forward demo ─────────────────────────────────────────────────────────

def run_walkforward(ticks: list, fast: int, slow: int, order_qty: int,
                    initial_cash: float, n_splits: int = 5):
    from quantsim.research.walkforward import WalkForward

    symbol = ticks[0].symbol if ticks else "?"

    def factory(_train):
        return EMACrossStrategy(symbol, fast=fast, slow=slow, order_qty=order_qty)

    wf      = WalkForward(n_splits=n_splits, train_frac=0.7, gap=0)
    results = wf.run(ticks, factory, initial_cash=initial_cash)
    summary = wf.summary(results)

    print(f"\n{'─'*52}")
    print(f"  Walk-Forward ({n_splits} windows)")
    print(f"{'─'*52}")
    for r in results:
        print(f"  Window {r['window']}: PnL=${r['final_pnl']:>10,.0f}  "
              f"Sharpe={r['sharpe']:>6.3f}  Fills={r['n_fills']}")
    print(f"{'─'*52}")
    print(f"  Mean Sharpe:  {summary.get('mean_sharpe', 0):.3f} "
          f"± {summary.get('std_sharpe', 0):.3f}")
    print(f"  Mean PnL:    ${summary.get('mean_pnl', 0):,.0f}")
    print(f"  % Positive:   {summary.get('pct_positive', 0):.1f}%")
    print(f"{'─'*52}\n")
    return summary


# ── Entry point ───────────────────────────────────────────────────────────────

def run(source: str = "synthetic", symbol: str = "AAPL",
        period: str = "1y", interval: str = "1d",
        fast: int = 10, slow: int = 30, order_qty: int = 100,
        n_ticks: int = 5000, seed: int = 42,
        tearsheet: bool = True, benchmark: bool = True,
        walkforward: bool = False) -> object:

    # ── Load data ─────────────────────────────────────────────────────────────
    if source == "yfinance":
        from quantsim.data.providers.yfinance_feed import YFinanceFeed
        print(f"[EMA] fetching {symbol} ({period}/{interval}) via Yahoo Finance…")
        ticks = YFinanceFeed().fetch(symbol, period=period, interval=interval)
    elif source == "alpaca":
        from quantsim.data.providers.alpaca_feed import AlpacaFeed
        print(f"[EMA] fetching {symbol} ({period}/{interval}) via Alpaca…")
        ticks = AlpacaFeed().fetch(symbol, period=period, interval=interval)
    else:
        from quantsim.data.synthetic import GBMFeed
        feed  = GBMFeed(symbol=symbol, initial_price=150.0, sigma=0.001, seed=seed)
        ticks = feed.generate(n_ticks)

    if not ticks:
        print("[EMA] No ticks loaded — aborting.")
        return None

    symbol = ticks[0].symbol

    # ── Walk-forward (optional) ───────────────────────────────────────────────
    if walkforward:
        run_walkforward(ticks, fast, slow, order_qty, initial_cash=500_000.0)

    # ── Full-period backtest ───────────────────────────────────────────────────
    stream = os.environ.get("QUANTSIM_STREAM") == "1"
    engine = BacktestEngine(initial_cash=500_000.0, stream_stdout=stream)
    result = engine.run(ticks, EMACrossStrategy(symbol, fast=fast, slow=slow,
                                                order_qty=order_qty))

    m = compute_metrics(result.equity)

    print(f"\n{'='*52}")
    print(f"  EMA Cross ({fast}/{slow}) — {symbol} ({source})")
    print(f"{'='*52}")
    print(f"  Final PnL:      ${result.final_pnl:,.2f}")
    print(f"  Total Return:    {m.get('total_return', 0)*100:.2f}%")
    print(f"  Sharpe (ann):    {m.get('sharpe', 0):.3f}")
    print(f"  Sortino:         {m.get('sortino', 0):.3f}")
    print(f"  Max Drawdown:    {m.get('max_drawdown', 0)*100:.2f}%")
    print(f"  Profit Factor:   {m.get('profit_factor', 0):.3f}")
    print(f"  Total Fills:     {len(result.fills)}")
    print(f"{'='*52}\n")

    # ── Tear-sheet ────────────────────────────────────────────────────────────
    if tearsheet:
        from quantsim.research.report import TearSheet, fetch_benchmark
        bm_prices = fetch_benchmark("SPY", period=period, interval=interval) \
                    if benchmark and source != "synthetic" else None
        ts = TearSheet(result, symbol=symbol, source=source,
                       benchmark_equity=bm_prices, benchmark_label="SPY")
        trades_path = ts.export_trades("trades.csv")
        sheet_path  = ts.save("tearsheet.png")
        print(f"  Tear-sheet → {sheet_path}")
        print(f"  Trades     → {trades_path}\n")

    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="EMA Crossover Strategy + Tear-Sheet")
    parser.add_argument("--source",        default="synthetic",
                        choices=["synthetic", "yfinance", "alpaca"])
    parser.add_argument("--symbol",        default="AAPL")
    parser.add_argument("--period",        default="1y")
    parser.add_argument("--interval",      default="1d")
    parser.add_argument("--fast",          type=int,   default=10)
    parser.add_argument("--slow",          type=int,   default=30)
    parser.add_argument("--qty",           type=int,   default=100)
    parser.add_argument("--ticks",         type=int,   default=5000)
    parser.add_argument("--seed",          type=int,   default=42)
    parser.add_argument("--walkforward",   action="store_true",
                        help="Run 5-window walk-forward test before full backtest")
    parser.add_argument("--no-tearsheet",  action="store_true")
    parser.add_argument("--no-benchmark",  action="store_true",
                        help="Skip SPY benchmark fetch")
    args = parser.parse_args()

    run(
        source      = args.source,
        symbol      = args.symbol,
        period      = args.period,
        interval    = args.interval,
        fast        = args.fast,
        slow        = args.slow,
        order_qty   = args.qty,
        n_ticks     = args.ticks,
        seed        = args.seed,
        tearsheet   = not args.no_tearsheet,
        benchmark   = not args.no_benchmark,
        walkforward = args.walkforward,
    )
