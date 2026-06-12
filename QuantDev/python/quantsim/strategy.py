"""
Strategy base class and event dataclasses.

Strategies receive events from the BacktestEngine or live sim engine and
issue orders via the provided gateway methods.
"""

from __future__ import annotations

from abc       import ABC, abstractmethod
from dataclasses import dataclass, field
from typing    import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from .backtester import BacktestEngine


# ── Event dataclasses ─────────────────────────────────────────────────────────

@dataclass(slots=True)
class BookUpdate:
    symbol:  str
    bid:     float
    ask:     float
    bid_sz:  int
    ask_sz:  int
    ts_ns:   int
    spread:  float = field(init=False)
    mid:     float = field(init=False)

    def __post_init__(self):
        self.spread = self.ask - self.bid
        self.mid    = (self.bid + self.ask) / 2.0


@dataclass(slots=True)
class FillEvent:
    order_id: int
    symbol:   str
    side:     str       # 'B' or 'S'
    qty:      int
    price:    float
    ts_ns:    int
    is_maker: bool = False


@dataclass(slots=True)
class OrderRef:
    order_id: int
    symbol:   str
    side:     str
    qty:      int
    price:    float
    ord_type: str       # 'LIMIT', 'MARKET', 'IOC', 'FOK'
    status:   str = 'PENDING'


# ── Strategy ABC ──────────────────────────────────────────────────────────────

class Strategy(ABC):
    """
    Base class for all strategies.

    Subclass and implement on_book_update(). Optionally override
    on_fill, on_timer, on_start, on_stop.

    Use send_order / cancel_order / cancel_all to interact with the engine.
    """

    def __init__(self):
        self._engine: Optional["BacktestEngine"] = None
        self._open_orders: dict[int, OrderRef] = {}

    # Called by engine to wire itself in
    def _attach(self, engine: "BacktestEngine"):
        self._engine = engine

    # ── Gateway API ───────────────────────────────────────────────────────────

    def send_order(self, symbol: str, side: str, qty: int,
                   price: float = 0.0, ord_type: str = "LIMIT") -> int:
        """Submit an order. Returns order_id (0 on reject)."""
        if self._engine is None:
            raise RuntimeError("Strategy not attached to engine")
        oid = self._engine._submit(symbol, side, qty, price, ord_type, self)
        if oid:
            self._open_orders[oid] = OrderRef(oid, symbol, side, qty, price, ord_type)
        return oid

    def cancel_order(self, order_id: int) -> bool:
        if self._engine is None:
            return False
        ok = self._engine._cancel(order_id)
        if ok:
            self._open_orders.pop(order_id, None)
        return ok

    def cancel_all(self, symbol: Optional[str] = None):
        ids = list(self._open_orders.keys())
        for oid in ids:
            ref = self._open_orders.get(oid)
            if ref and (symbol is None or ref.symbol == symbol):
                self.cancel_order(oid)

    @property
    def open_orders(self) -> dict[int, OrderRef]:
        return dict(self._open_orders)

    # ── Engine callbacks ──────────────────────────────────────────────────────

    @abstractmethod
    def on_book_update(self, update: BookUpdate): ...

    def on_fill(self, fill: FillEvent):
        self._open_orders.pop(fill.order_id, None)

    def on_timer(self, ts_ns: int): pass
    def on_start(self): pass
    def on_stop(self):  pass
