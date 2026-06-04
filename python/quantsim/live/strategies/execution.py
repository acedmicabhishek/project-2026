"""
TWAP execution algorithm.

Works a parent order (buy or sell a target quantity) evenly over a time budget,
minimizing market impact. Posts passively at the touch to capture spread; if it
falls behind schedule it crosses to catch up. Reports implementation shortfall
vs the arrival mid at the end.

This is an *execution* strategy, not an alpha strategy — the benchmark is price,
not PnL. It is how a desk fills a large client order without moving the market.
"""

from __future__ import annotations

import time

from . import LiveStrategy
from ...strategy import BookUpdate


class TWAPExecution(LiveStrategy):
    def __init__(self,
                 side:        str   = "B",       # parent side
                 target_qty:  float = 0.05,      # total to execute
                 duration_s:  float = 120.0,     # time budget
                 n_slices:    int   = 20,
                 catch_up_bps: float = 5.0):     # cross if behind by this much
        super().__init__()
        self.side        = "B" if side in ("B", "BUY", "buy") else "S"
        self.target_qty  = target_qty
        self.duration_s  = duration_s
        self.n_slices    = max(1, n_slices)
        self.slice_qty   = target_qty / self.n_slices
        self.catch_up    = catch_up_bps / 1e4

        self._filled     = 0.0
        self._t0         = 0.0
        self._arrival_mid = 0.0
        self._working_id = 0
        self.shortfall_bps = 0.0    # implementation shortfall vs arrival (filled at end)

    def on_start(self) -> None:
        self._t0 = time.monotonic()

    def on_fill(self, fill) -> None:
        super().on_fill(fill)
        self._filled += fill.qty
        self._working_id = 0

    def on_book_update(self, u: BookUpdate) -> None:
        book = self.book
        if book is None or book.mid <= 0:
            return
        if self._arrival_mid == 0.0:
            self._arrival_mid = book.mid
        if self._filled >= self.target_qty - 1e-9:
            return

        elapsed = time.monotonic() - self._t0
        frac    = min(1.0, elapsed / self.duration_s) if self.duration_s > 0 else 1.0
        target_by_now = frac * self.target_qty
        behind = target_by_now - self._filled

        remaining = self.target_qty - self._filled
        if remaining <= 0:
            return

        # if we have a working order and we're on schedule, let it sit
        if self._working_id and behind < self.slice_qty:
            return

        self.cancel_all()
        self._working_id = 0
        qty = min(max(self.slice_qty, behind), remaining)

        if self.side == "B":
            # behind schedule a lot -> cross the spread to catch up
            if behind > self.slice_qty * 2:
                self._working_id = self.market_buy(qty)
            else:
                self._working_id = self.limit_buy(qty, book.best_bid, post_only=True)
        else:
            if behind > self.slice_qty * 2:
                self._working_id = self.market_sell(qty)
            else:
                self._working_id = self.limit_sell(qty, book.best_ask, post_only=True)

    def on_stop(self) -> None:
        if self._arrival_mid > 0 and self._filled > 0:
            book = self.book
            last_mid = book.mid if book else self._arrival_mid
            sign = 1 if self.side == "B" else -1
            # shortfall: positive = worse than arrival mid
            self.shortfall_bps = sign * (last_mid - self._arrival_mid) / self._arrival_mid * 1e4
