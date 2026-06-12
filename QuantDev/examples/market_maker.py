"""
Avellaneda-Stoikov market making strategy.

Usage:
    python examples/market_maker.py --ticks 5000 --sigma 0.0008 --gamma 0.1

Implements:
  - Reservation price:   r = mid - q * γ * σ² * T
  - Optimal spread:      δ = γ * σ² * T + (2/γ) * ln(1 + γ/κ)
  - Inventory skew:      cancel/replace quotes when inventory exceeds threshold
  - Stale quote protection: cancel if mid moves > reprice_threshold
"""

import argparse
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))

from quantsim.strategy   import Strategy, BookUpdate, FillEvent
from quantsim.backtester import BacktestEngine, BacktestResult
from quantsim.data.synthetic import GBMFeed
from quantsim.analytics  import compute_metrics, plot_pnl

import math


class AvellanedaStoikov(Strategy):
    """
    Market making using the Avellaneda-Stoikov model.
    """

    def __init__(self, symbol: str,
                 gamma:   float = 0.1,    # risk aversion
                 sigma:   float = 0.12,   # dollar volatility per tick (price × log-vol)
                 kappa:   float = 50.0,   # order arrival rate (higher → tighter spread)
                 T:       float = 1.0,    # time horizon (normalized)
                 max_inventory: int = 500,
                 reprice_threshold: float = 0.005,  # cancel if mid moves > this
                 order_qty: int = 100):
        super().__init__()
        self.symbol             = symbol
        self.gamma              = gamma
        self.sigma              = sigma
        self.kappa              = kappa
        self.T                  = T
        self.max_inventory      = max_inventory
        self.reprice_threshold  = reprice_threshold
        self.order_qty          = order_qty

        self._inventory    = 0
        self._last_mid     = 0.0
        self._bid_id       = 0
        self._ask_id       = 0
        self._tick_count   = 0
        self._total_ticks  = 1000  # rough estimate for time_remaining

    def reservation_price(self, mid: float) -> float:
        q = self._inventory
        time_rem = max(0.01, 1.0 - self._tick_count / self._total_ticks)
        return mid - q * self.gamma * self.sigma**2 * time_rem

    def optimal_spread(self) -> float:
        time_rem = max(0.01, 1.0 - self._tick_count / self._total_ticks)
        half_spread = (self.gamma * self.sigma**2 * time_rem / 2.0 +
                       math.log(1.0 + self.gamma / self.kappa) / self.gamma)
        return max(0.005, half_spread)

    def on_start(self):
        print(f"[MM] Starting Avellaneda-Stoikov on {self.symbol}")

    def on_book_update(self, update: BookUpdate):
        self._tick_count += 1
        mid = update.mid

        # Inventory hard limit: stop quoting
        if abs(self._inventory) >= self.max_inventory:
            self.cancel_all(self.symbol)
            self._bid_id = self._ask_id = 0
            return

        r      = self.reservation_price(mid)
        spread = self.optimal_spread()
        new_bid = round(r - spread, 4)
        new_ask = round(r + spread, 4)

        # Reprice if mid moved significantly
        reprice = (self._last_mid == 0.0 or
                   abs(mid - self._last_mid) > self.reprice_threshold)

        if reprice:
            # Cancel existing quotes
            if self._bid_id:
                self.cancel_order(self._bid_id)
                self._bid_id = 0
            if self._ask_id:
                self.cancel_order(self._ask_id)
                self._ask_id = 0

            # Place new quotes
            if new_bid > 0:
                self._bid_id = self.send_order(
                    self.symbol, 'B', self.order_qty, new_bid, 'LIMIT')
            self._ask_id = self.send_order(
                self.symbol, 'S', self.order_qty, new_ask, 'LIMIT')

            self._last_mid = mid

    def on_fill(self, fill: FillEvent):
        super().on_fill(fill)
        delta = fill.qty if fill.side in ('B', 'BUY') else -fill.qty
        self._inventory += delta
        # Immediately replace filled side
        if fill.order_id == self._bid_id:
            self._bid_id = 0
        elif fill.order_id == self._ask_id:
            self._ask_id = 0


def _load_ticks(source: str, symbol: str, n_ticks: int,
                sigma: float, seed: int, period: str, interval: str):
    if source == "yfinance":
        from quantsim.data.providers.yfinance_feed import YFinanceFeed
        print(f"[MM] fetching {symbol} ({period}/{interval}) from Yahoo Finance…")
        return YFinanceFeed().fetch(symbol, period=period, interval=interval)
    if source == "alpaca":
        from quantsim.data.providers.alpaca_feed import AlpacaFeed
        print(f"[MM] fetching {symbol} ({period}/{interval}) from Alpaca…")
        return AlpacaFeed().fetch(symbol, period=period, interval=interval)
    # default: synthetic GBM
    feed = GBMFeed(symbol=symbol, initial_price=150.0, sigma=sigma, seed=seed)
    return feed.generate(n_ticks)


def run(n_ticks: int = 5000, sigma: float = 0.0008, gamma: float = 0.1,
        seed: int = 42, plot: bool = False, source: str = "synthetic",
        symbol: str = "AAPL", period: str = "1y", interval: str = "1d"):
    ticks  = _load_ticks(source, symbol, n_ticks, sigma, seed, period, interval)
    symbol = ticks[0].symbol if ticks else symbol

    import os
    stream = os.environ.get("QUANTSIM_STREAM") == "1"
    engine = BacktestEngine(initial_cash=500_000.0, stream_stdout=stream)
    result = engine.run(ticks, AvellanedaStoikov(symbol, gamma=gamma, sigma=sigma))

    m = compute_metrics(result.equity)
    print(f"\n{'='*50}")
    print(f"  Market Maker Backtest Results  ({n_ticks} ticks)")
    print(f"{'='*50}")
    print(f"  Final PnL:      ${result.final_pnl:,.2f}")
    print(f"  Sharpe:          {m.get('sharpe', 0):.3f}")
    print(f"  Max Drawdown:    {m.get('max_drawdown', 0)*100:.2f}%")
    print(f"  Profit Factor:   {m.get('profit_factor', 0):.3f}")
    print(f"  Total Fills:     {len(result.fills)}")
    print(f"  Open Position:   {result.open_pos}")
    print(f"  Data Source:     {source} / {symbol}")
    print(f"{'='*50}\n")

    if plot:
        plot_pnl(result.equity, title=f"Avellaneda-Stoikov MM — {symbol}")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Avellaneda-Stoikov Market Maker Backtest")
    parser.add_argument("--ticks",    type=int,   default=5000)
    parser.add_argument("--sigma",    type=float, default=0.0008)
    parser.add_argument("--gamma",    type=float, default=0.1)
    parser.add_argument("--seed",     type=int,   default=42)
    parser.add_argument("--plot",     action="store_true")
    parser.add_argument("--source",   default="synthetic",
                        choices=["synthetic", "yfinance", "alpaca"],
                        help="Data source (default: synthetic GBM)")
    parser.add_argument("--symbol",   default="AAPL",
                        help="Ticker symbol (default: AAPL)")
    parser.add_argument("--period",   default="1y",
                        help="History period for real data (default: 1y)")
    parser.add_argument("--interval", default="1d",
                        help="Bar interval for real data (default: 1d)")
    args = parser.parse_args()
    run(args.ticks, args.sigma, args.gamma, args.seed, args.plot,
        args.source, args.symbol, args.period, args.interval)
