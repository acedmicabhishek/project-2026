"""
Live strategy library.

All strategies subclass `LiveStrategy`, which extends the base `Strategy` with:
  - self.book        : the full L2 LiveBook (set by the engine each tick)
  - self.net_position: current signed position (read from the engine)
  - convenience order helpers (limit/market/post-only buy/sell)

Registry maps a short id -> class so the CLI and GUI can launch by name.
"""

from __future__ import annotations

from typing import Optional

from ...strategy import Strategy, BookUpdate
from ..feed      import LiveBook, Trade


class LiveStrategy(Strategy):
    """Base for live crypto strategies."""

    price_dp: int = 2     # price decimals (BTCUSDT tick = 0.01)
    qty_dp:   int = 6     # quantity decimals

    def __init__(self):
        super().__init__()
        self.book: Optional[LiveBook] = None

    # ── position view (engine owns the truth) ─────────────────────────────────

    @property
    def net_position(self) -> float:
        eng = self._engine
        return getattr(eng, "pos").net if eng is not None and hasattr(eng, "pos") else 0.0

    @property
    def avg_px(self) -> float:
        eng = self._engine
        return getattr(eng, "pos").avg_px if eng is not None and hasattr(eng, "pos") else 0.0

    # ── order helpers ─────────────────────────────────────────────────────────

    def _r(self, px: float) -> float:
        return round(px, self.price_dp)

    def _q(self, q: float) -> float:
        return round(q, self.qty_dp)

    def limit_buy(self, qty: float, price: float, post_only: bool = False) -> int:
        return self.send_order(self.book.symbol, "B", self._q(qty), self._r(price),
                               "POST_ONLY" if post_only else "LIMIT")

    def limit_sell(self, qty: float, price: float, post_only: bool = False) -> int:
        return self.send_order(self.book.symbol, "S", self._q(qty), self._r(price),
                               "POST_ONLY" if post_only else "LIMIT")

    def market_buy(self, qty: float) -> int:
        return self.send_order(self.book.symbol, "B", self._q(qty), 0.0, "MARKET")

    def market_sell(self, qty: float) -> int:
        return self.send_order(self.book.symbol, "S", self._q(qty), 0.0, "MARKET")


from .market_maker   import MarketMaker        # noqa: E402
from .momentum       import Momentum           # noqa: E402
from .mean_reversion import MeanReversion      # noqa: E402
from .execution      import TWAPExecution      # noqa: E402

REGISTRY = {
    "mm":        MarketMaker,
    "momentum":  Momentum,
    "meanrev":   MeanReversion,
    "twap":      TWAPExecution,
}

__all__ = ["LiveStrategy", "MarketMaker", "Momentum", "MeanReversion",
           "TWAPExecution", "REGISTRY"]
