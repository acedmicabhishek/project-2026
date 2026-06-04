"""
TWAP execution algorithm.

Splits a parent order into equal-sized slices over equal time intervals.
Reports implementation shortfall vs. arrival price.

Usage:
    python examples/twap.py --qty 10000 --slices 10 --ticks 2000
"""

import argparse
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))

from quantsim.strategy   import Strategy, BookUpdate, FillEvent
from quantsim.backtester import BacktestEngine
from quantsim.data.synthetic import GBMFeed
from quantsim.analytics  import compute_metrics


class TWAPStrategy(Strategy):
    """
    TWAP: fires equal-sized slices at equal tick intervals.
    Tracks implementation shortfall vs arrival price.
    """

    def __init__(self, symbol: str, total_qty: int, side: str,
                 n_slices: int = 10, ticks_per_slice: int = 100):
        super().__init__()
        self.symbol          = symbol
        self.total_qty       = total_qty
        self.side            = side
        self.n_slices        = n_slices
        self.ticks_per_slice = ticks_per_slice

        self._slice_qty      = total_qty // n_slices
        self._slices_sent    = 0
        self._filled_qty     = 0
        self._tick_count     = 0
        self._arrival_price  = 0.0
        self._fill_notional  = 0.0
        self._active         = True

    def on_start(self):
        print(f"[TWAP] {self.side} {self.total_qty} {self.symbol} "
              f"over {self.n_slices} slices × {self.ticks_per_slice} ticks each")

    def on_book_update(self, update: BookUpdate):
        if update.symbol != self.symbol:
            return
        if not self._active:
            return

        self._tick_count += 1

        # Record arrival price on first tick
        if self._tick_count == 1:
            self._arrival_price = update.mid

        # Fire slice at interval
        if (self._tick_count % self.ticks_per_slice == 0 and
                self._slices_sent < self.n_slices):

            remaining_slices = self.n_slices - self._slices_sent
            remaining_qty    = self.total_qty - self._filled_qty

            # Last slice: send all remaining
            qty = (remaining_qty if self._slices_sent == self.n_slices - 1
                   else self._slice_qty)

            price = update.ask if self.side == 'B' else update.bid
            self.send_order(self.symbol, self.side, qty, price, 'LIMIT')
            self._slices_sent += 1

            if self._slices_sent >= self.n_slices:
                self._active = False
                self._report()

    def on_fill(self, fill: FillEvent):
        super().on_fill(fill)
        if fill.symbol == self.symbol:
            self._filled_qty    += fill.qty
            self._fill_notional += fill.qty * fill.price

    def _report(self):
        if self._filled_qty == 0:
            print("[TWAP] No fills")
            return
        avg_fill = self._fill_notional / self._filled_qty
        slippage_bps = ((avg_fill - self._arrival_price) / self._arrival_price * 10000
                         if self.side == 'B'
                         else (self._arrival_price - avg_fill) / self._arrival_price * 10000)
        print(f"[TWAP] Done. Filled: {self._filled_qty}/{self.total_qty}")
        print(f"       Arrival: {self._arrival_price:.4f}  Avg fill: {avg_fill:.4f}")
        print(f"       IS (slippage): {slippage_bps:.2f} bps")


def run(total_qty: int = 10_000, n_slices: int = 10, n_ticks: int = 2000,
        seed: int = 42, source: str = "synthetic", symbol: str = "TSLA",
        period: str = "1y", interval: str = "1d"):
    if source == "yfinance":
        from quantsim.data.providers.yfinance_feed import YFinanceFeed
        print(f"[TWAP] fetching {symbol} ({period}/{interval}) from Yahoo Finance…")
        ticks = YFinanceFeed().fetch(symbol, period=period, interval=interval)
    elif source == "alpaca":
        from quantsim.data.providers.alpaca_feed import AlpacaFeed
        print(f"[TWAP] fetching {symbol} ({period}/{interval}) from Alpaca…")
        ticks = AlpacaFeed().fetch(symbol, period=period, interval=interval)
    else:
        feed  = GBMFeed(symbol=symbol, initial_price=200.0, sigma=0.001, seed=seed)
        ticks = feed.generate(n_ticks)

    symbol = ticks[0].symbol if ticks else symbol
    n_ticks_actual = len(ticks)

    import os
    stream = os.environ.get("QUANTSIM_STREAM") == "1"
    engine = BacktestEngine(initial_cash=5_000_000.0, stream_stdout=stream)
    result = engine.run(ticks, TWAPStrategy(
        symbol, total_qty=total_qty, side='B',
        n_slices=n_slices,
        ticks_per_slice=max(1, n_ticks_actual // n_slices),
    ))

    print(f"\n{'='*50}")
    print(f"  TWAP Execution Results  ({source} / {symbol})")
    print(f"{'='*50}")
    print(f"  Final cash PnL:  ${result.final_pnl:,.2f}")
    print(f"  Total Fills:     {len(result.fills)}")
    print(f"  Open Position:   {result.open_pos}")
    print(f"{'='*50}\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="TWAP Execution Backtest")
    parser.add_argument("--qty",      type=int, default=10_000)
    parser.add_argument("--slices",   type=int, default=10)
    parser.add_argument("--ticks",    type=int, default=2000)
    parser.add_argument("--seed",     type=int, default=42)
    parser.add_argument("--source",   default="synthetic",
                        choices=["synthetic", "yfinance", "alpaca"])
    parser.add_argument("--symbol",   default="TSLA")
    parser.add_argument("--period",   default="1y")
    parser.add_argument("--interval", default="1d")
    args = parser.parse_args()
    run(args.qty, args.slices, args.ticks, args.seed,
        args.source, args.symbol, args.period, args.interval)
