"""
Microprice mean reversion (maker).

Tracks the z-score of the mid against a rolling mean. When price stretches far
from the mean it fades the move with a passive limit order on the cheap side,
then exits as the z-score reverts toward zero. Profits from overreaction; loses
in strong trends — the inverse risk profile of momentum.

  z = (mid - rolling_mean) / rolling_std
  z < -entry : buy   (oversold)   |  z > +entry : sell (overbought)
  |z| < exit : flatten
"""

from __future__ import annotations

from collections import deque

from . import LiveStrategy
from ...strategy import BookUpdate


class MeanReversion(LiveStrategy):
    def __init__(self,
                 window:       int   = 120,
                 entry_z:      float = 2.0,
                 exit_z:       float = 0.4,
                 trade_size:   float = 0.003,
                 max_position: float = 0.01):
        super().__init__()
        self.window       = window
        self.entry_z      = entry_z
        self.exit_z       = exit_z
        self.trade_size   = trade_size
        self.max_position = max_position
        self._mids = deque(maxlen=window)

    def on_book_update(self, u: BookUpdate) -> None:
        book = self.book
        if book is None or book.mid <= 0:
            return
        mid = book.mid
        self._mids.append(mid)
        if len(self._mids) < self.window:
            return

        mean = sum(self._mids) / len(self._mids)
        var  = sum((m - mean) ** 2 for m in self._mids) / len(self._mids)
        std  = var ** 0.5
        if std <= 0:
            return
        z = (mid - mean) / std

        pos = self.net_position

        # exit toward flat when reverted
        if abs(z) < self.exit_z and abs(pos) > 1e-9:
            self.cancel_all()
            if pos > 0:
                self.limit_sell(pos, book.best_bid)     # lift our own side off the bid
            else:
                self.limit_buy(-pos, book.best_ask)
            return

        # entry: fade the stretch with a passive order
        if z <= -self.entry_z and pos < self.max_position:
            self.cancel_all()
            self.limit_buy(self.trade_size, book.best_bid, post_only=True)
        elif z >= self.entry_z and pos > -self.max_position:
            self.cancel_all()
            self.limit_sell(self.trade_size, book.best_ask, post_only=True)
