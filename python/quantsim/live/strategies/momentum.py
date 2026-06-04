"""
Order-flow momentum (taker).

Combines a fast/slow EMA crossover on the mid with order-book imbalance as a
confirming filter, then takes liquidity to hold a directional position. This is
the simplest "alpha" strategy: predict the next move, pay the spread to be in it.

  signal = sign(ema_fast - ema_slow)  gated by imbalance over top N levels
  target = signal * max_position
  trade  = market order to close the gap to target
"""

from __future__ import annotations

from . import LiveStrategy
from ...strategy import BookUpdate


class Momentum(LiveStrategy):
    def __init__(self,
                 fast:        int   = 20,
                 slow:        int   = 60,
                 imb_levels:  int   = 5,
                 imb_thresh:  float = 0.15,
                 max_position: float = 0.01,
                 trade_size:  float = 0.002,
                 min_trade:   float = 0.0005):
        super().__init__()
        self.alpha_f = 2.0 / (fast + 1)
        self.alpha_s = 2.0 / (slow + 1)
        self.imb_levels  = imb_levels
        self.imb_thresh  = imb_thresh
        self.max_position = max_position
        self.trade_size  = trade_size
        self.min_trade   = min_trade
        self._ema_f = 0.0
        self._ema_s = 0.0
        self._n = 0

    def on_book_update(self, u: BookUpdate) -> None:
        book = self.book
        if book is None or book.mid <= 0:
            return
        mid = book.mid

        # seed then update EMAs
        if self._n == 0:
            self._ema_f = self._ema_s = mid
        else:
            self._ema_f += self.alpha_f * (mid - self._ema_f)
            self._ema_s += self.alpha_s * (mid - self._ema_s)
        self._n += 1
        if self._n < 60:                      # warm up
            return

        imb = book.imbalance(self.imb_levels)
        trend = self._ema_f - self._ema_s

        signal = 0
        if trend > 0 and imb > self.imb_thresh:
            signal = 1
        elif trend < 0 and imb < -self.imb_thresh:
            signal = -1

        target = signal * self.max_position
        diff   = target - self.net_position
        if abs(diff) < max(self.min_trade, self.trade_size * 0.5):
            return

        size = min(abs(diff), self.trade_size)
        if diff > 0:
            self.market_buy(size)
        else:
            self.market_sell(size)
