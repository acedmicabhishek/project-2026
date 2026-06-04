"""
Avellaneda-Stoikov-style market maker (crypto spot), anchored to the real touch.

Earlier this quoted a fixed bps half-spread (e.g. 4 bps ≈ $25 on BTC) which sat
~$12 off the touch and never filled — flat PnL. Real crypto spreads are a tick or
two wide, so quotes must be placed relative to the *touch*, in ticks:

  - spread wide enough (≥3 ticks): improve by one tick (join the front of an empty
    in-spread level → reliable fills, capture spread − 2 ticks).
  - tight (1–2 ticks): join best bid/ask (queue behind resting size; fills as the
    real tape trades through — slower, but honest).
  - inventory skew: back the quote that would grow inventory off the touch by a few
    ticks, so the book naturally pulls the position toward flat.

Post-only throughout, so it always earns the maker side. Stops quoting a side at
the inventory cap.
"""

from __future__ import annotations

from . import LiveStrategy
from ...strategy import BookUpdate


class MarketMaker(LiveStrategy):
    def __init__(self,
                 quote_size:    float = 0.002,   # base units per quote (e.g. BTC)
                 spread_bps:    float = 4.0,     # accepted for CLI compat (unused)
                 gamma:         float = 0.6,     # inventory risk aversion (skew strength)
                 max_inventory: float = 0.02,    # |position| cap
                 improve_min_ticks: int = 3):    # improve only if spread ≥ this
        super().__init__()
        self.quote_size    = quote_size
        self.gamma         = gamma
        self.max_inventory = max_inventory
        self.improve_min   = improve_min_ticks
        self._last_bb = 0.0
        self._last_ba = 0.0
        self._bid_id = 0
        self._ask_id = 0

    def on_book_update(self, u: BookUpdate) -> None:
        book = self.book
        if book is None or book.best_bid <= 0 or book.best_ask <= 0:
            return

        tick = 10.0 ** (-self.price_dp)
        bb, ba = book.best_bid, book.best_ask

        # requote only when the touch moves (limits churn / order rate)
        if bb == self._last_bb and ba == self._last_ba:
            return
        self._last_bb, self._last_ba = bb, ba

        spread_ticks = max(1, round((ba - bb) / tick))
        inv          = self.net_position
        inv_ratio    = inv / self.max_inventory if self.max_inventory > 0 else 0.0

        # base placement: improve inside a wide spread, else join the touch
        if spread_ticks >= self.improve_min:
            bid_px = bb + tick
            ask_px = ba - tick
        else:
            bid_px = bb
            ask_px = ba

        # inventory skew (in ticks): back off the side that grows the position
        skew_ticks = int(round(self.gamma * abs(inv_ratio) * spread_ticks))
        if inv_ratio > 0:        # long -> buy less eagerly
            bid_px -= skew_ticks * tick
        elif inv_ratio < 0:      # short -> sell less eagerly
            ask_px += skew_ticks * tick

        # never invert our own quotes
        if bid_px >= ask_px:
            bid_px, ask_px = bb, ba

        self.cancel_all()
        self._bid_id = self._ask_id = 0
        if inv < self.max_inventory:                     # room to buy
            self._bid_id = self.limit_buy(self.quote_size, bid_px, post_only=True)
        if inv > -self.max_inventory:                    # room to sell
            self._ask_id = self.limit_sell(self.quote_size, ask_px, post_only=True)
