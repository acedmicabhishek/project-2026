"""
Normalized live-feed types + abstract feed interface.

A feed emits a stream of `MarketEvent`s — either a `LiveBook` (full L2 snapshot
of the top N levels) or a `Trade` (one print off the tape). Every venue adapter
(Binance, and later others) normalizes into these same types so the engine and
strategies are venue-agnostic.
"""

from __future__ import annotations

import time
from abc          import ABC, abstractmethod
from dataclasses  import dataclass, field
from typing       import AsyncIterator, List, Literal, Optional, Tuple, Union


# ── Order book level ──────────────────────────────────────────────────────────

@dataclass(slots=True)
class BookLevel:
    price: float
    qty:   float


# ── L2 book snapshot ──────────────────────────────────────────────────────────

@dataclass(slots=True)
class LiveBook:
    """Top-N L2 order book snapshot. bids descending, asks ascending."""
    symbol: str
    ts_ns:  int
    bids:   List[BookLevel]
    asks:   List[BookLevel]
    recv_ns: int = 0          # local receive time (for feed-latency measurement)

    @property
    def best_bid(self) -> float:
        return self.bids[0].price if self.bids else 0.0

    @property
    def best_ask(self) -> float:
        return self.asks[0].price if self.asks else 0.0

    @property
    def best_bid_qty(self) -> float:
        return self.bids[0].qty if self.bids else 0.0

    @property
    def best_ask_qty(self) -> float:
        return self.asks[0].qty if self.asks else 0.0

    @property
    def mid(self) -> float:
        if self.bids and self.asks:
            return (self.bids[0].price + self.asks[0].price) / 2.0
        return self.best_bid or self.best_ask

    @property
    def spread(self) -> float:
        if self.bids and self.asks:
            return self.asks[0].price - self.bids[0].price
        return 0.0

    @property
    def spread_bps(self) -> float:
        m = self.mid
        return (self.spread / m * 1e4) if m > 0 else 0.0

    @property
    def microprice(self) -> float:
        """Size-weighted mid: leans toward the side with less size (about to move)."""
        if not (self.bids and self.asks):
            return self.mid
        bq, aq = self.bids[0].qty, self.asks[0].qty
        tot = bq + aq
        if tot <= 0:
            return self.mid
        # weight each side's *price* by the *opposite* side's size
        return (self.bids[0].price * aq + self.asks[0].price * bq) / tot

    def imbalance(self, levels: int = 5) -> float:
        """
        Order-book imbalance over the top `levels`.
        Returns (bid_vol - ask_vol) / (bid_vol + ask_vol) in [-1, 1].
        Positive = buy pressure.
        """
        bid_vol = sum(l.qty for l in self.bids[:levels])
        ask_vol = sum(l.qty for l in self.asks[:levels])
        tot = bid_vol + ask_vol
        return (bid_vol - ask_vol) / tot if tot > 0 else 0.0

    def depth_value(self, levels: int = 10) -> Tuple[float, float]:
        """Notional value resting on each side within `levels` (bid_notional, ask_notional)."""
        bid_n = sum(l.price * l.qty for l in self.bids[:levels])
        ask_n = sum(l.price * l.qty for l in self.asks[:levels])
        return bid_n, ask_n


# ── Trade print ───────────────────────────────────────────────────────────────

@dataclass(slots=True)
class Trade:
    """A single trade off the tape."""
    symbol:          str
    ts_ns:           int
    price:           float
    qty:             float
    is_buyer_maker:  bool       # True  -> sell aggressor (traded at the bid)
                                # False -> buy  aggressor (traded at the ask)
    recv_ns: int = 0

    @property
    def aggressor(self) -> Literal["B", "S"]:
        return "S" if self.is_buyer_maker else "B"

    @property
    def signed_qty(self) -> float:
        """+qty if buy-initiated, -qty if sell-initiated."""
        return -self.qty if self.is_buyer_maker else self.qty


MarketEvent = Union[LiveBook, Trade]


# ── Abstract feed ─────────────────────────────────────────────────────────────

class LiveFeed(ABC):
    """Abstract real-time market data feed."""

    @abstractmethod
    def events(self) -> AsyncIterator[MarketEvent]:
        """Async-iterate normalized market events until cancelled."""
        ...

    @property
    @abstractmethod
    def symbol(self) -> str:
        ...


def now_ns() -> int:
    """Wall-clock time in nanoseconds since epoch."""
    return time.time_ns()
