"""
Statistical arbitrage / pairs trading strategy.

Uses OU-process spread between two correlated assets.
Entry: z-score > entry_z (short spread) or < -entry_z (long spread)
Exit:  z-score crosses 0
Stop:  z-score > stop_z

Usage:
    python examples/stat_arb.py --ticks 5000 --entry_z 2.0
"""

import argparse
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))

from quantsim.strategy   import Strategy, BookUpdate, FillEvent
from quantsim.backtester import BacktestEngine
from quantsim.data.synthetic import OUSpreadFeed, GBMFeed
from quantsim.analytics  import compute_metrics

import math
from collections import deque


class ZScoreCalculator:
    """Rolling z-score of a time series."""

    def __init__(self, window: int = 60):
        self.window = window
        self._buf   = deque(maxlen=window)

    def update(self, x: float) -> float:
        self._buf.append(x)
        if len(self._buf) < self.window:
            return 0.0
        arr = list(self._buf)
        mean = sum(arr) / len(arr)
        var  = sum((v - mean)**2 for v in arr) / len(arr)
        std  = math.sqrt(var) if var > 0 else 1e-9
        return (x - mean) / std

    @property
    def ready(self) -> bool:
        return len(self._buf) >= self.window


class StatArbStrategy(Strategy):
    """
    Pairs trading using rolling z-score of price spread.
    Trades symbol_a vs symbol_b (hedged 1:1).
    """

    def __init__(self, symbol_a: str, symbol_b: str,
                 entry_z: float = 2.0, exit_z: float = 0.3,
                 stop_z:  float = 4.0, window: int = 60,
                 order_qty: int = 100):
        super().__init__()
        self.sym_a      = symbol_a
        self.sym_b      = symbol_b
        self.entry_z    = entry_z
        self.exit_z     = exit_z
        self.stop_z     = stop_z
        self.order_qty  = order_qty

        self._zscore    = ZScoreCalculator(window)
        self._state     = 'flat'  # 'flat', 'long_spread', 'short_spread'
        self._mids: dict = {}
        self._pos_a = 0
        self._pos_b = 0

    def _spread(self) -> float:
        a = self._mids.get(self.sym_a, 0.0)
        b = self._mids.get(self.sym_b, 0.0)
        return a - b if b > 0 else 0.0

    def on_book_update(self, update: BookUpdate):
        self._mids[update.symbol] = update.mid

        # Need both prices before trading
        if self.sym_a not in self._mids or self.sym_b not in self._mids:
            return

        spread = self._spread()
        z = self._zscore.update(spread)

        if not self._zscore.ready:
            return

        if self._state == 'flat':
            if z > self.entry_z:
                # Spread too high: short A, long B
                self.send_order(self.sym_a, 'S', self.order_qty,
                                self._mids[self.sym_a], 'MARKET')
                self.send_order(self.sym_b, 'B', self.order_qty,
                                self._mids[self.sym_b], 'MARKET')
                self._state = 'short_spread'

            elif z < -self.entry_z:
                # Spread too low: long A, short B
                self.send_order(self.sym_a, 'B', self.order_qty,
                                self._mids[self.sym_a], 'MARKET')
                self.send_order(self.sym_b, 'S', self.order_qty,
                                self._mids[self.sym_b], 'MARKET')
                self._state = 'long_spread'

        elif self._state == 'short_spread':
            # Exit: z crosses below exit_z or stop
            if abs(z) < self.exit_z or z < -self.stop_z:
                self._exit()

        elif self._state == 'long_spread':
            if abs(z) < self.exit_z or z > self.stop_z:
                self._exit()

    def _exit(self):
        mid_a = self._mids.get(self.sym_a, 0.0)
        mid_b = self._mids.get(self.sym_b, 0.0)

        if self._state == 'short_spread':
            self.send_order(self.sym_a, 'B', self.order_qty, mid_a, 'MARKET')
            self.send_order(self.sym_b, 'S', self.order_qty, mid_b, 'MARKET')
        elif self._state == 'long_spread':
            self.send_order(self.sym_a, 'S', self.order_qty, mid_a, 'MARKET')
            self.send_order(self.sym_b, 'B', self.order_qty, mid_b, 'MARKET')

        self._state = 'flat'

    def on_fill(self, fill: FillEvent):
        super().on_fill(fill)
        delta = fill.qty if fill.side in ('B', 'BUY') else -fill.qty
        if fill.symbol == self.sym_a:
            self._pos_a += delta
        else:
            self._pos_b += delta


def _load_pair(source: str, sym_a: str, sym_b: str, n_ticks: int,
               seed: int, period: str, interval: str):
    if source in ("yfinance", "alpaca"):
        if source == "yfinance":
            from quantsim.data.providers.yfinance_feed import YFinanceFeed
            feed_cls = YFinanceFeed
        else:
            from quantsim.data.providers.alpaca_feed import AlpacaFeed
            feed_cls = AlpacaFeed
        f = feed_cls()
        print(f"[SA] fetching {sym_a}/{sym_b} ({period}/{interval}) from {source}…")
        ta = f.fetch(sym_a, period=period, interval=interval)
        tb = f.fetch(sym_b, period=period, interval=interval)
        return ta, tb
    # default: OU synthetic
    feed = OUSpreadFeed(
        symbol_a=sym_a, symbol_b=sym_b,
        initial_price=450.0,
        theta=0.08, mu_spread=0.0, sigma_spread=0.05,
        sigma_price=0.001, seed=seed,
    )
    return feed.generate(n_ticks)


def run(n_ticks: int = 5000, entry_z: float = 2.0, seed: int = 42, plot: bool = False,
        source: str = "synthetic", sym_a: str = "SPY", sym_b: str = "QQQ",
        period: str = "1y", interval: str = "1d"):
    ticks_a, ticks_b = _load_pair(source, sym_a, sym_b, n_ticks, seed, period, interval)

    # Interleave both symbols' ticks chronologically
    combined = sorted(ticks_a + ticks_b, key=lambda t: t.ts_ns)

    import os
    stream = os.environ.get("QUANTSIM_STREAM") == "1"
    engine = BacktestEngine(initial_cash=500_000.0, stream_stdout=stream)
    result = engine.run(combined, StatArbStrategy(sym_a, sym_b, entry_z=entry_z))

    m = compute_metrics(result.equity)
    n_combined = len(combined)
    print(f"\n{'='*50}")
    print(f"  Stat Arb Backtest Results  ({n_combined} ticks, {source})")
    print(f"{'='*50}")
    print(f"  Final PnL:    ${result.final_pnl:,.2f}")
    print(f"  Sharpe:        {m.get('sharpe', 0):.3f}")
    print(f"  Max DD:        {m.get('max_drawdown', 0)*100:.2f}%")
    print(f"  Total Fills:   {len(result.fills)}")
    print(f"  Open Pos:      {result.open_pos}")
    print(f"{'='*50}\n")

    if plot:
        from quantsim.analytics import plot_pnl
        plot_pnl(result.equity, title="Stat Arb PnL")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Stat Arb Backtest")
    parser.add_argument("--ticks",    type=int,   default=5000)
    parser.add_argument("--entry_z",  type=float, default=2.0)
    parser.add_argument("--seed",     type=int,   default=42)
    parser.add_argument("--plot",     action="store_true")
    parser.add_argument("--source",   default="synthetic",
                        choices=["synthetic", "yfinance", "alpaca"])
    parser.add_argument("--sym_a",    default="SPY")
    parser.add_argument("--sym_b",    default="QQQ")
    parser.add_argument("--period",   default="1y")
    parser.add_argument("--interval", default="1d")
    args = parser.parse_args()
    run(args.ticks, args.entry_z, args.seed, args.plot,
        args.source, args.sym_a, args.sym_b, args.period, args.interval)

