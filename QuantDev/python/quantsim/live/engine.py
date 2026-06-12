"""
Live trading engine.

Drives a Strategy on a live feed: reconstructs the book via the gateway, runs
pre-trade risk on every order, tracks position + PnL honestly, and streams the
whole session to the GUI over the protocol.

    feed ──events──> LiveEngine ──BookUpdate──> Strategy
                         │  ▲                      │
                    gateway  └────── fills ────────┘ send_order
                         │
                      protocol ──stdout──> GUI

The Strategy surface is identical to the backtester (send_order / cancel_order /
on_book_update / on_fill), so a strategy written for backtest runs live unchanged.
Live-aware strategies can additionally read `self.book` (full L2 LiveBook).
"""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field
from typing      import Optional

from ..strategy import Strategy, BookUpdate, FillEvent
from .feed      import LiveBook, Trade, MarketEvent, now_ns
from .gateway   import PaperGateway, GatewayFill
from .risk      import RiskManager, RiskLimits
from .protocol  import StreamWriter


# ── Position / PnL ────────────────────────────────────────────────────────────

class Position:
    """Signed-position accounting with average cost basis and realized PnL."""

    __slots__ = ("net", "avg_px", "realized", "fees")

    def __init__(self):
        self.net      = 0.0
        self.avg_px   = 0.0
        self.realized = 0.0
        self.fees     = 0.0

    def apply(self, side: str, qty: float, price: float, fee: float) -> None:
        sign = 1.0 if side in ("B", "BUY", "buy") else -1.0
        self.fees     += fee
        self.realized -= fee
        qty_rem = qty

        # reduce/close an opposite position first
        if self.net != 0.0 and (self.net > 0.0) != (sign > 0.0):
            closing = min(qty_rem, abs(self.net))
            if self.net > 0.0:
                self.realized += (price - self.avg_px) * closing
            else:
                self.realized += (self.avg_px - price) * closing
            self.net += sign * closing
            qty_rem  -= closing
            if abs(self.net) < 1e-12:
                self.net = 0.0
                self.avg_px = 0.0

        # remaining opens or increases position in sign direction
        if qty_rem > 1e-12:
            new_abs = abs(self.net) + qty_rem
            self.avg_px = ((self.avg_px * abs(self.net) + price * qty_rem) / new_abs
                           if new_abs > 0 else price)
            self.net += sign * qty_rem

    def unrealized(self, mid: float) -> float:
        return (mid - self.avg_px) * self.net if self.net != 0.0 else 0.0


# ── Config ────────────────────────────────────────────────────────────────────

@dataclass
class LiveConfig:
    symbol:          str   = "BTCUSDT"
    initial_cash:    float = 100_000.0
    duration_s:      float = 0.0        # 0 = run until externally stopped
    maker_fee_bps:   float = 0.0
    taker_fee_bps:   float = 5.0
    stream:          bool  = True
    stat_interval_s: float = 1.0
    depth_levels:    int   = 12
    risk: RiskLimits = field(default_factory=RiskLimits)


# ── Engine ────────────────────────────────────────────────────────────────────

class LiveEngine:
    def __init__(self, feed, strategy: Strategy, config: Optional[LiveConfig] = None,
                 writer: Optional[StreamWriter] = None):
        self.feed   = feed
        self.cfg    = config or LiveConfig(symbol=getattr(feed, "symbol", "BTCUSDT"))
        self.strat  = strategy
        self.gw     = PaperGateway(self.cfg.maker_fee_bps, self.cfg.taker_fee_bps)
        self.risk   = RiskManager(self.cfg.risk)
        self.out    = writer or StreamWriter(enabled=self.cfg.stream)
        self.pos    = Position()

        self.gw.on_fill = self._on_fill
        self.strat._attach(self)

        self._oid          = 0
        self.book: Optional[LiveBook] = None
        self.last_mid      = 0.0
        self.equity_curve  = []
        self.orders_sent   = 0
        self.orders_blocked = 0
        self.fills_total   = 0
        self.maker_fills   = 0
        self._stop         = False
        self._last_stat    = 0.0
        self._t0           = 0.0

    # ── Strategy gateway surface (matches BacktestEngine) ─────────────────────

    def _submit(self, symbol: str, side: str, qty, price: float,
                ord_type: str, strategy: Strategy) -> int:
        if self.book is None:
            return 0
        qty = float(qty)
        is_buy = side in ("B", "BUY", "buy")
        post_only = ord_type.upper() == "POST_ONLY"
        ot = "LIMIT" if post_only else ord_type.upper()

        ok, reason = self.risk.check(is_buy, qty, price, self.book.mid,
                                     self.pos.net, ot)
        if not ok:
            self.orders_blocked += 1
            self.out.risk("BLOCKED", reason)
            return 0

        self._oid += 1
        oid = self._oid
        self.orders_sent += 1
        self.gw.submit(oid, symbol, side, qty, price, ot, post_only=post_only)
        return oid

    def _cancel(self, order_id: int) -> bool:
        return self.gw.cancel(order_id)

    # ── Fill handling ─────────────────────────────────────────────────────────

    def _on_fill(self, gf: GatewayFill) -> None:
        self.pos.apply(gf.side, gf.qty, gf.price, gf.fee)
        self.fills_total += 1
        if gf.is_maker:
            self.maker_fills += 1
        self.out.fill(gf.side, gf.qty, gf.price, gf.symbol, gf.is_maker)
        self._emit_position()
        ev = FillEvent(order_id=gf.order_id, symbol=gf.symbol, side=gf.side,
                       qty=gf.qty, price=gf.price, ts_ns=gf.ts_ns,
                       is_maker=gf.is_maker)
        try:
            self.strat.on_fill(ev)
        except Exception as e:  # noqa: BLE001 — a strategy bug must not kill the session
            self.out.log(f"strategy.on_fill error: {e}")

    def _emit_position(self) -> None:
        unreal = self.pos.unrealized(self.last_mid)
        self.out.position(self.pos.net, self.pos.avg_px, self.pos.realized, unreal)

    # ── Main loop ─────────────────────────────────────────────────────────────

    def stop(self) -> None:
        self._stop = True
        if hasattr(self.feed, "stop"):
            self.feed.stop()

    async def run(self) -> dict:
        self.strat.on_start()
        self._t0 = time.monotonic()
        self.out.log(f"live session start: {self.cfg.symbol} "
                     f"cash=${self.cfg.initial_cash:,.0f} "
                     f"strategy={type(self.strat).__name__}")
        self.out.risk(self.risk.state, "")

        try:
            async for ev in self.feed.events():
                if self._stop:
                    break
                if isinstance(ev, LiveBook):
                    self._on_book(ev)
                elif isinstance(ev, Trade):
                    self._on_trade(ev)

                if self.cfg.duration_s > 0 and \
                        (time.monotonic() - self._t0) >= self.cfg.duration_s:
                    break
        except asyncio.CancelledError:
            pass
        finally:
            self.strat.on_stop()

        return self._finalize()

    def _on_book(self, book: LiveBook) -> None:
        self.gw.on_book(book)
        self.book = book
        self.last_mid = book.mid
        # expose richer book to live-aware strategies
        setattr(self.strat, "book", book)

        bu = BookUpdate(symbol=book.symbol, bid=book.best_bid, ask=book.best_ask,
                        bid_sz=int(round(book.best_bid_qty)),
                        ask_sz=int(round(book.best_ask_qty)), ts_ns=book.ts_ns)
        try:
            self.strat.on_book_update(bu)
        except Exception as e:  # noqa: BLE001
            self.out.log(f"strategy.on_book_update error: {e}")

        unreal = self.pos.unrealized(self.last_mid)
        eq = self.cfg.initial_cash + self.pos.realized + unreal
        self.equity_curve.append(eq)
        self.risk.on_pnl(eq - self.cfg.initial_cash)

        # stream the frame
        self.out.equity(eq)
        self.out.mid(book.mid)
        self.out.best_quote(book.best_bid, book.best_ask)
        self.out.depth([(l.price, l.qty) for l in book.bids],
                       [(l.price, l.qty) for l in book.asks],
                       self.cfg.depth_levels)
        self.out.latency((now_ns() - book.recv_ns) / 1000.0)

        if self.risk.kill_armed:
            self.gw.cancel_all()
            self.out.risk("ARMED", self.risk.kill_reason)

        now = time.monotonic()
        if now - self._last_stat >= self.cfg.stat_interval_s:
            self._emit_stats(eq)
            self._last_stat = now

    def _on_trade(self, trade: Trade) -> None:
        self.gw.on_trade(trade)         # drives maker fills off the real tape
        on_trade = getattr(self.strat, "on_trade", None)
        if callable(on_trade):
            try:
                on_trade(trade)
            except Exception as e:  # noqa: BLE001
                self.out.log(f"strategy.on_trade error: {e}")

    # ── Stats / finalize ──────────────────────────────────────────────────────

    def _emit_stats(self, eq: float) -> None:
        from ..analytics import compute_metrics
        m = compute_metrics(self.equity_curve) if len(self.equity_curve) > 2 else {}
        maker_ratio = (self.maker_fills / self.fills_total) if self.fills_total else 0.0
        self.out.stat(
            sharpe=f"{m.get('sharpe', 0.0):.3f}",
            sortino=f"{m.get('sortino', 0.0):.3f}",
            max_dd=f"{m.get('max_drawdown', 0.0):.4f}",
            vol=f"{m.get('volatility', 0.0):.4f}",
            pnl=f"{eq - self.cfg.initial_cash:.2f}",
            realized=f"{self.pos.realized:.2f}",
            fees=f"{self.pos.fees:.2f}",
            pos=f"{self.pos.net:.6f}",
            fills=str(self.fills_total),
            orders=str(self.orders_sent),
            blocked=str(self.orders_blocked),
            maker_ratio=f"{maker_ratio:.3f}",
            open_orders=str(self.gw.open_orders),
        )

    def _finalize(self) -> dict:
        from ..analytics import compute_metrics
        eq = self.equity_curve[-1] if self.equity_curve else self.cfg.initial_cash
        m = compute_metrics(self.equity_curve) if len(self.equity_curve) > 2 else {}
        self._emit_stats(eq)
        self.out.log(f"session end: pnl=${eq - self.cfg.initial_cash:,.2f} "
                     f"fills={self.fills_total} orders={self.orders_sent} "
                     f"blocked={self.orders_blocked}")
        return {
            "pnl":      eq - self.cfg.initial_cash,
            "equity":   self.equity_curve,
            "fills":    self.fills_total,
            "orders":   self.orders_sent,
            "blocked":  self.orders_blocked,
            "position": self.pos.net,
            "realized": self.pos.realized,
            "fees":     self.pos.fees,
            "metrics":  m,
        }
