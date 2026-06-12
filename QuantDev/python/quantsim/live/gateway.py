"""
Paper trading gateway.

Fills orders against the *live* reconstructed book and the *real* trade tape —
not a synthetic counterparty. This makes paper fills far more honest than a
probabilistic backtest model:

  Taker  (market / aggressive limit): walks the real book levels, pays the real
         offered prices + taker fee. Models slippage through depth.

  Maker  (passive limit): rests in a local queue. Fills when EITHER
           (a) the real book trades through the price (book-cross), OR
           (b) a real trade prints at the price with the correct aggressor and
               our queue position has been exhausted.
         Queue position = the size that was resting ahead of us at our price
         when we joined. Real trades at that price consume it first (FIFO),
         which is exactly how a real exchange queue behaves.

This is a simulation gateway. Swapping in a real exchange gateway (REST/WS order
entry) means implementing the same `submit / cancel` surface — the engine above
it does not change.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing      import Callable, Dict, List, Optional

from .feed import LiveBook, Trade


@dataclass(slots=True)
class GatewayFill:
    order_id: int
    symbol:   str
    side:     str        # 'B' or 'S'
    qty:      float
    price:    float
    is_maker: bool
    fee:      float
    ts_ns:    int


@dataclass(slots=True)
class _Resting:
    order_id:    int
    symbol:      str
    is_buy:      bool
    price:       float
    qty:         float        # remaining
    queue_ahead: float        # size that must trade at our price before us
    ts_ns:       int
    post_only:   bool = False


class PaperGateway:
    """
    Simulated order gateway driven by live book + trade tape.

    Call order:
        gw.on_book(book)            # every L2 snapshot
        gw.on_trade(trade)          # every tape print
        gw.submit(...)              # from the strategy
        gw.cancel(order_id)
    Fills are delivered through the `on_fill` callback (set by the engine).
    """

    def __init__(self,
                 maker_fee_bps: float = 0.0,    # Binance VIP0 spot maker ~1.0; 0 = post-only rebate era
                 taker_fee_bps: float = 5.0,    # Binance VIP0 spot taker ~5.0 bps
                 latency_ns:    int   = 0):
        self.maker_fee = maker_fee_bps / 1e4
        self.taker_fee = taker_fee_bps / 1e4
        self.latency_ns = latency_ns

        self.on_fill: Optional[Callable[[GatewayFill], None]] = None

        self._book:   Optional[LiveBook] = None
        self._resting: Dict[int, _Resting] = {}
        self._seq = 0

    # ── market data in ────────────────────────────────────────────────────────

    def on_book(self, book: LiveBook) -> None:
        self._book = book
        self._check_book_cross()

    def on_trade(self, trade: Trade) -> None:
        self._consume_tape(trade)

    @property
    def book(self) -> Optional[LiveBook]:
        return self._book

    @property
    def open_orders(self) -> int:
        return len(self._resting)

    # ── order entry ───────────────────────────────────────────────────────────

    def submit(self, order_id: int, symbol: str, side: str, qty: float,
               price: float = 0.0, ord_type: str = "LIMIT",
               post_only: bool = False) -> List[GatewayFill]:
        """
        Route an order. Returns immediate (taker) fills; passive remainder rests
        and fills later via on_book / on_trade. Maker fills also arrive through
        the on_fill callback.
        """
        is_buy   = side in ("B", "BUY", "buy")
        ord_type = ord_type.upper()
        book     = self._book
        fills: List[GatewayFill] = []

        if book is None:
            return fills

        best_bid, best_ask = book.best_bid, book.best_ask

        if ord_type == "MARKET":
            fills = self._take(order_id, symbol, is_buy, qty, ts_ns=book.ts_ns)

        elif ord_type in ("IOC", "FOK"):
            # marketable up to limit price; FOK requires full size available
            limit = price if price > 0 else (best_ask if is_buy else best_bid)
            avail = self._marketable_qty(is_buy, limit)
            if ord_type == "FOK" and avail < qty:
                return fills  # killed
            take_qty = min(qty, avail)
            if take_qty > 0:
                fills = self._take(order_id, symbol, is_buy, take_qty,
                                   limit=limit, ts_ns=book.ts_ns)

        else:  # LIMIT
            crosses = (is_buy and price >= best_ask > 0) or \
                      (not is_buy and 0 < price <= best_bid)
            if crosses and post_only:
                return fills  # post-only would take → reject
            if crosses:
                fills = self._take(order_id, symbol, is_buy, qty,
                                   limit=price, ts_ns=book.ts_ns)
                rem = qty - sum(f.qty for f in fills)
                if rem > 1e-12:
                    self._rest(order_id, symbol, is_buy, price, rem, post_only)
            else:
                self._rest(order_id, symbol, is_buy, price, qty, post_only)

        for f in fills:
            self._emit(f)
        return fills

    def cancel(self, order_id: int) -> bool:
        return self._resting.pop(order_id, None) is not None

    def cancel_all(self) -> None:
        self._resting.clear()

    # ── taker matching: walk the real book ───────────────────────────────────

    def _marketable_qty(self, is_buy: bool, limit: float) -> float:
        book = self._book
        if book is None:
            return 0.0
        levels = book.asks if is_buy else book.bids
        q = 0.0
        for lvl in levels:
            if (is_buy and lvl.price <= limit) or (not is_buy and lvl.price >= limit):
                q += lvl.qty
            else:
                break
        return q

    def _take(self, order_id: int, symbol: str, is_buy: bool, qty: float,
              limit: Optional[float] = None, ts_ns: int = 0) -> List[GatewayFill]:
        book = self._book
        out: List[GatewayFill] = []
        if book is None:
            return out
        levels = book.asks if is_buy else book.bids
        remaining = qty
        for lvl in levels:
            if remaining <= 1e-12:
                break
            if limit is not None:
                if is_buy and lvl.price > limit:   break
                if not is_buy and lvl.price < limit: break
            take = min(remaining, lvl.qty)
            if take <= 0:
                continue
            fee = take * lvl.price * self.taker_fee
            out.append(GatewayFill(order_id, symbol, "B" if is_buy else "S",
                                   take, lvl.price, is_maker=False, fee=fee, ts_ns=ts_ns))
            remaining -= take
        return out

    # ── maker resting + queue model ──────────────────────────────────────────

    def _rest(self, order_id: int, symbol: str, is_buy: bool,
              price: float, qty: float, post_only: bool) -> None:
        book = self._book
        queue_ahead = 0.0
        if book is not None:
            levels = book.bids if is_buy else book.asks
            for lvl in levels:
                if abs(lvl.price - price) < 1e-9:
                    queue_ahead = lvl.qty   # size already resting at our price
                    break
        self._resting[order_id] = _Resting(
            order_id=order_id, symbol=symbol, is_buy=is_buy, price=price,
            qty=qty, queue_ahead=queue_ahead,
            ts_ns=book.ts_ns if book else 0, post_only=post_only)

    def _check_book_cross(self) -> None:
        """If the market has moved through a resting order, fill it (maker price)."""
        book = self._book
        if book is None or not self._resting:
            return
        best_bid, best_ask = book.best_bid, book.best_ask
        done: List[int] = []
        for oid, o in self._resting.items():
            crossed = (o.is_buy and 0 < best_ask <= o.price) or \
                      (not o.is_buy and best_bid >= o.price > 0)
            if crossed:
                fee = o.qty * o.price * self.maker_fee
                self._emit(GatewayFill(oid, o.symbol, "B" if o.is_buy else "S",
                                       o.qty, o.price, is_maker=True, fee=fee,
                                       ts_ns=book.ts_ns))
                done.append(oid)
        for oid in done:
            self._resting.pop(oid, None)

    def _consume_tape(self, trade: Trade) -> None:
        """
        A real trade printed. Decrement queue ahead of resting orders at that
        price; fill the overflow. Sell-initiated trades (is_buyer_maker=True)
        hit resting BIDS; buy-initiated trades hit resting ASKS.
        """
        if not self._resting:
            return
        hits_bids = trade.is_buyer_maker          # sell aggressor consumes bids
        done: List[int] = []
        for oid, o in self._resting.items():
            # a trade interacts with our order only on the matching side+price
            if o.is_buy and not hits_bids:
                continue
            if (not o.is_buy) and hits_bids:
                continue
            interacts = (o.is_buy and trade.price <= o.price) or \
                        ((not o.is_buy) and trade.price >= o.price)
            if not interacts:
                continue

            vol = trade.qty
            if o.queue_ahead > 0:
                consumed = min(o.queue_ahead, vol)
                o.queue_ahead -= consumed
                vol -= consumed
            if vol > 1e-12 and o.queue_ahead <= 1e-12:
                fill_qty = min(o.qty, vol)
                o.qty -= fill_qty
                fee = fill_qty * o.price * self.maker_fee
                self._emit(GatewayFill(oid, o.symbol, "B" if o.is_buy else "S",
                                       fill_qty, o.price, is_maker=True, fee=fee,
                                       ts_ns=trade.ts_ns))
                if o.qty <= 1e-12:
                    done.append(oid)
        for oid in done:
            self._resting.pop(oid, None)

    def _emit(self, fill: GatewayFill) -> None:
        if self.on_fill:
            self.on_fill(fill)
