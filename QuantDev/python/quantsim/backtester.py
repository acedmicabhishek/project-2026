"""
Event-driven backtesting engine.

Flow per tick:
  TickEvent → update internal LOB/mid → fire on_book_update(strategy)
  → strategy calls send_order → match against simulated book
  → fire on_fill(strategy) → update positions / PnL

Also supports WalkForward and PurgedKFold cross-validation.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing      import Callable, List, Optional, Sequence, Dict, Tuple
import numpy as np

from .strategy import Strategy, BookUpdate, FillEvent, OrderRef

try:
    from quantsim_core import (
        MatchingEngine, Order, OrdType, Side, OrdStatus,
        TickEvent, SyntheticFeed, SyntheticParams, TradingMetrics,
        KillSwitch,
    )
    CORE_AVAILABLE = True
except ImportError:
    CORE_AVAILABLE = False


# ── BacktestResult ────────────────────────────────────────────────────────────

@dataclass
class BacktestResult:
    fills:       List[FillEvent]       = field(default_factory=list)
    equity:      List[float]           = field(default_factory=list)
    timestamps:  List[int]             = field(default_factory=list)
    final_pnl:   float                 = 0.0
    metrics:     Dict[str, float]      = field(default_factory=dict)
    open_pos:    Dict[str, int]        = field(default_factory=dict)   # symbol → net qty

    @property
    def returns(self) -> np.ndarray:
        eq = np.array(self.equity)
        return np.diff(eq) / np.where(eq[:-1] != 0, eq[:-1], 1.0)


# ── BacktestEngine ────────────────────────────────────────────────────────────

class BacktestEngine:
    """
    Event-driven backtest engine. Uses C++ MatchingEngine when available,
    otherwise pure-Python fill simulation.

    Usage:
        engine = BacktestEngine()
        result = engine.run(ticks, MyStrategy())
    """

    def __init__(self,
                 initial_cash:         float = 1_000_000.0,
                 timer_interval_ns:    int   = 0,
                 latency_ns:           int   = 0,
                 commission_per_share: float = 0.005,
                 slippage_ticks:       int   = 0,
                 stream_stdout:        bool  = False):
        self.initial_cash        = initial_cash
        self.timer_interval_ns   = timer_interval_ns
        self.latency_ns          = latency_ns
        self.commission_per_share = commission_per_share
        self.slippage_ticks      = slippage_ticks
        self._stream             = stream_stdout

        self._engine    = MatchingEngine() if CORE_AVAILABLE else None
        self._strategy: Optional[Strategy] = None
        self._order_seq = 0
        self._pending_orders: Dict[int, Tuple[int, object]] = {}  # id → (fire_ts, order)

        # Pure-Python resting limit order book
        # {oid: {'symbol', 'side', 'qty', 'price', 'ts_ns'}}
        self._resting: Dict[int, dict] = {}

        # Position tracking
        self._positions: Dict[str, int]   = {}   # symbol → net qty
        self._cash      = initial_cash
        self._pnl       = 0.0
        self._fills:    List[FillEvent]   = []
        self._equity:   List[float]       = []
        self._ts_log:   List[int]         = []

        # Mid-price cache for PnL marking
        self._mids:  Dict[str, float] = {}
        self._bids:  Dict[str, float] = {}
        self._asks:  Dict[str, float] = {}

        if self._engine:
            self._engine.on_fill        = self._on_cpp_fill
            self._engine.on_book_update = self._on_cpp_book_update

        self._last_timer_ns = 0
        self._last_snap: Optional[BookUpdate] = None

    # ── Main run loop ─────────────────────────────────────────────────────────

    def run(self, ticks: Sequence, strategy: Strategy) -> BacktestResult:
        self._strategy = strategy
        strategy._attach(self)
        strategy.on_start()

        self._fills.clear()
        self._equity.clear()
        self._ts_log.clear()
        self._positions.clear()
        self._resting.clear()
        self._cash  = self.initial_cash
        self._pnl   = 0.0
        self._mids  = {}
        self._bids  = {}
        self._asks  = {}

        for tick in ticks:
            # Accept both TickEvent (C++ obj) and plain dicts
            ts_ns  = getattr(tick, 'ts_ns',  0) if not isinstance(tick, dict) else tick.get('ts_ns', 0)
            symbol = getattr(tick, 'symbol', '') if not isinstance(tick, dict) else tick.get('symbol', '')
            bid    = getattr(tick, 'bid',    0.0) if not isinstance(tick, dict) else tick.get('bid', 0.0)
            ask    = getattr(tick, 'ask',    0.0) if not isinstance(tick, dict) else tick.get('ask', 0.0)
            bsz    = getattr(tick, 'size',   100) if not isinstance(tick, dict) else tick.get('size', 100)

            if bid <= 0 or ask <= 0 or ask < bid:
                continue

            mid = (bid + ask) / 2.0
            self._mids[symbol] = mid
            self._bids[symbol] = bid
            self._asks[symbol] = ask

            # Check resting passive limit orders for fill against new bid/ask.
            # Always run fill simulation — C++ engine handles crossing orders,
            # but passive maker fills are simulated here for backtest realism.
            self._fill_resting(symbol, bid, ask, ts_ns)

            # Fire pending orders whose latency has elapsed
            self._process_pending_orders(ts_ns)

            # Timer callback
            if self.timer_interval_ns > 0:
                if ts_ns - self._last_timer_ns >= self.timer_interval_ns:
                    strategy.on_timer(ts_ns)
                    self._last_timer_ns = ts_ns

            # Book update callback
            update = BookUpdate(symbol=symbol, bid=bid, ask=ask,
                                bid_sz=bsz, ask_sz=bsz, ts_ns=ts_ns)
            self._last_snap = update
            strategy.on_book_update(update)

            # Mark-to-market equity
            eq = self._mark_equity()
            self._equity.append(eq)
            self._ts_log.append(ts_ns)
            if self._stream:
                import sys
                sys.stdout.write(f"EQ:{eq:.4f}\nBA:{bid:.4f},{ask:.4f}\n")
                sys.stdout.flush()

        strategy.on_stop()

        from .analytics import compute_metrics
        final_eq  = self._equity[-1] if self._equity else self.initial_cash
        result = BacktestResult(
            fills      = self._fills[:],
            equity     = self._equity[:],
            timestamps = self._ts_log[:],
            final_pnl  = final_eq - self.initial_cash,
            open_pos   = dict(self._positions),
        )
        if len(result.equity) > 2:
            result.metrics = compute_metrics(result.equity)
        return result

    # ── Gateway methods (called by Strategy) ─────────────────────────────────

    def _submit(self, symbol: str, side: str, qty: int,
                price: float, ord_type: str, strategy: Strategy) -> int:
        self._order_seq += 1
        oid = self._order_seq
        ctx = self._mk_order_ctx(symbol, side, qty, price, ord_type)

        # MARKET/IOC/FOK always use Python path for immediate fills.
        # The C++ LOB has no synthetic counterparty, so market orders
        # sent to the C++ engine would never match.
        # LIMIT orders use C++ engine when available for LOB tracking.
        if self._engine is None or ord_type in ('MARKET', 'IOC', 'FOK'):
            self._submit_python(oid, ctx, strategy)
        else:
            self._submit_cpp(oid, ctx)

        return oid

    def _mk_order_ctx(self, symbol: str, side: str, qty: int,
                      price: float, ord_type: str) -> dict:
        """Shared state needed by both submit paths."""
        bid    = self._bids.get(symbol, self._last_snap.bid if self._last_snap else 0.0)
        ask    = self._asks.get(symbol, self._last_snap.ask if self._last_snap else 0.0)
        mid    = self._mids.get(symbol, price if price > 0 else 0.0)
        is_buy = side in ('B', 'BUY', 'buy')
        ts_now = self._last_snap.ts_ns if self._last_snap else 0
        tick   = max(ask - bid, 0.01)
        return dict(symbol=symbol, side=side, qty=qty, price=price,
                    ord_type=ord_type, bid=bid, ask=ask, mid=mid,
                    is_buy=is_buy, ts_now=ts_now, tick=tick)

    def _immediate_fill(self, oid: int, ctx: dict, fill_price: float,
                        strategy: Optional[Strategy]) -> None:
        """Fire a taker fill and notify strategy."""
        commission = ctx['qty'] * self.commission_per_share
        fill = FillEvent(order_id=oid, symbol=ctx['symbol'], side=ctx['side'],
                         qty=ctx['qty'], price=fill_price, ts_ns=ctx['ts_now'])
        self._apply_fill(fill, commission)
        if strategy:
            strategy.on_fill(fill)

    def _rest_order(self, oid: int, ctx: dict) -> None:
        """Add a passive limit order to the resting book."""
        self._resting[oid] = {
            'symbol': ctx['symbol'], 'side': ctx['side'],
            'qty': ctx['qty'],       'price': ctx['price'],
            'ts_ns': ctx['ts_now'],
        }

    def _submit_python(self, oid: int, ctx: dict, strategy: Optional[Strategy]) -> None:
        """Pure-Python order routing: market sim fills, no C++ engine."""
        bid, ask, mid  = ctx['bid'], ctx['ask'], ctx['mid']
        is_buy         = ctx['is_buy']
        ord_type       = ctx['ord_type']

        if ord_type == 'MARKET':
            fp = (ask if is_buy else bid) + self.slippage_ticks * ctx['tick'] * (1 if is_buy else -1)
            self._immediate_fill(oid, ctx, fp or mid, strategy)

        elif ord_type in ('IOC', 'FOK'):
            fp = ask if is_buy else bid
            if fp > 0:
                self._immediate_fill(oid, ctx, fp, strategy)

        else:  # LIMIT
            if is_buy and ctx['price'] >= ask > 0:
                self._immediate_fill(oid, ctx, ask, strategy)   # aggressive cross
            elif not is_buy and ctx['price'] <= bid > 0:
                self._immediate_fill(oid, ctx, bid, strategy)   # aggressive cross
            else:
                self._rest_order(oid, ctx)                       # passive maker

    def _submit_cpp(self, oid: int, ctx: dict) -> None:
        """Route order through C++ matching engine."""
        bid, ask = ctx['bid'], ctx['ask']
        is_buy   = ctx['is_buy']

        order = Order()
        order.id     = oid
        order.symbol = ctx['symbol']
        order.side   = Side.Buy if is_buy else Side.Sell
        order.price  = ctx['price']
        order.qty    = ctx['qty']
        order.type   = {
            'LIMIT':  OrdType.Limit,
            'MARKET': OrdType.Market,
            'IOC':    OrdType.IOC,
            'FOK':    OrdType.FOK,
        }.get(ctx['ord_type'].upper(), OrdType.Limit)

        # Also track passive limits in _resting for fill simulation.
        # C++ engine handles crossing fills; passive maker fills come
        # from _fill_resting's probabilistic model.
        if ctx['ord_type'] == 'LIMIT':
            crosses = (is_buy and ctx['price'] >= ask > 0) or \
                      (not is_buy and ctx['price'] <= bid > 0)
            if not crosses:
                self._rest_order(oid, ctx)

        if self.latency_ns > 0 and self._last_snap:
            self._pending_orders[oid] = (self._last_snap.ts_ns + self.latency_ns, order)
        else:
            self._engine.submit(order)

        return oid

    def _cancel(self, order_id: int) -> bool:
        self._pending_orders.pop(order_id, None)
        self._resting.pop(order_id, None)      # remove from pure-Python resting book
        if self._engine:
            return self._engine.cancel(order_id)
        return True

    def _fill_resting(self, symbol: str, bid: float, ask: float, ts_ns: int):
        """
        Check resting passive limit orders for fills.

        Fill model (two tiers):
          1. Definite: market has crossed the limit price (ask <= buy_price or
             bid >= sell_price). Fill at own limit price (maker rebate).
          2. Probabilistic: order is within 10 bps of the best quote.
             Per-tick fill probability = exp(-50 * distance_in_dollars).
             This approximates Poisson order-flow arrival at the quote.
        """
        filled = []
        for oid, o in self._resting.items():
            if o['symbol'] != symbol:
                continue
            is_buy = o['side'] in ('B', 'BUY', 'buy')
            fill_price = None

            if is_buy:
                if ask <= o['price']:
                    # Market crossed our bid
                    fill_price = o['price']
                else:
                    # Probabilistic: how close is ask to our bid?
                    dist = ask - o['price']          # > 0, order is below ask
                    prob = np.exp(-50.0 * dist)      # ~37% at 2 cents away
                    if np.random.random() < prob:
                        fill_price = o['price']
            else:
                if bid >= o['price']:
                    fill_price = o['price']
                else:
                    dist = o['price'] - bid
                    prob = np.exp(-50.0 * dist)
                    if np.random.random() < prob:
                        fill_price = o['price']

            if fill_price is not None:
                commission = o['qty'] * self.commission_per_share
                fill = FillEvent(order_id=oid, symbol=symbol, side=o['side'],
                                 qty=o['qty'], price=fill_price, ts_ns=ts_ns)
                self._apply_fill(fill, commission)
                if self._strategy:
                    self._strategy.on_fill(fill)
                if self._stream:
                    import sys
                    sys.stdout.write(
                        f"FILL:{o['side']}:{o['qty']}:{fill_price:.4f}:{symbol}\n"
                    )
                    sys.stdout.flush()
                filled.append(oid)

        for oid in filled:
            self._resting.pop(oid)
            # Sync with C++ LOB: cancel the order from the engine so the LOB
            # remains consistent after a simulated passive fill.
            if self._engine:
                self._engine.cancel(oid)

    def _process_pending_orders(self, now_ts: int):
        ready = [oid for oid, (fire_ts, _) in self._pending_orders.items()
                 if fire_ts <= now_ts]
        for oid in ready:
            _, order = self._pending_orders.pop(oid)
            if self._engine:
                self._engine.submit(order)

    # ── C++ callbacks ─────────────────────────────────────────────────────────

    def _on_cpp_fill(self, cpp_fill):
        # Determine if our strategy was the taker
        side = 'B' if cpp_fill.aggressor == Side.Buy else 'S'
        fill = FillEvent(
            order_id = cpp_fill.taker_id,
            symbol   = self._last_snap.symbol if self._last_snap else '',
            side     = side,
            qty      = cpp_fill.qty,
            price    = cpp_fill.price,
            ts_ns    = cpp_fill.ts_ns,
            is_maker = False,
        )
        self._apply_fill(fill)
        if self._strategy:
            self._strategy.on_fill(fill)

    def _on_cpp_book_update(self, snap):
        pass  # handled in main loop

    # ── Position / PnL tracking ───────────────────────────────────────────────

    def _apply_fill(self, fill: FillEvent, commission: float = 0.0):
        self._fills.append(fill)
        delta = fill.qty if fill.side in ('B', 'BUY') else -fill.qty
        self._positions[fill.symbol] = self._positions.get(fill.symbol, 0) + delta

        # Cash flow: buy → spend, sell → receive, minus transaction costs
        cash_delta = -fill.qty * fill.price if fill.side in ('B', 'BUY') \
                     else fill.qty * fill.price
        self._cash += cash_delta - commission

    def _mark_equity(self) -> float:
        pos_value = sum(
            qty * self._mids.get(sym, 0.0)
            for sym, qty in self._positions.items()
        )
        return self._cash + pos_value


# ── Walk-Forward Testing ──────────────────────────────────────────────────────

class WalkForwardTest:
    """
    Anchored or rolling walk-forward test.
    strategy_factory: callable() → Strategy instance
    data: list/array of ticks
    """

    def __init__(self, train_window: int = 252, test_window: int = 63,
                 anchored: bool = False):
        self.train_window = train_window
        self.test_window  = test_window
        self.anchored     = anchored

    def run(self, strategy_factory: Callable, data: Sequence,
            engine_kwargs: dict = {}) -> List[BacktestResult]:
        results = []
        n = len(data)
        start = self.train_window
        while start + self.test_window <= n:
            train_start = 0 if self.anchored else start - self.train_window
            train_data = data[train_start:start]
            test_data  = data[start:start + self.test_window]

            strategy = strategy_factory()
            # Allow strategy to fit on training data
            if hasattr(strategy, 'fit'):
                strategy.fit(train_data)

            engine = BacktestEngine(**engine_kwargs)
            result = engine.run(test_data, strategy)
            results.append(result)
            start += self.test_window

        return results


# ── Purged K-Fold Cross-Validation ────────────────────────────────────────────

class PurgedKFold:
    """
    Combinatorial Purged K-Fold CV for time-series features.
    Removes training samples whose labels overlap with test window.
    """

    def __init__(self, n_splits: int = 5, embargo: float = 0.01):
        self.n_splits = n_splits
        self.embargo  = embargo

    def split(self, X: np.ndarray,
              pred_times: np.ndarray,
              eval_times: np.ndarray):
        """
        Yields (train_idx, test_idx) pairs.
        pred_times: when each sample was generated
        eval_times: when each sample's label ends
        """
        n = len(X)
        indices = np.arange(n)
        embargo_gap = int(n * self.embargo)

        splits = np.array_split(indices, self.n_splits)

        for i, test_idx in enumerate(splits):
            test_start = pred_times[test_idx[0]]
            test_end   = eval_times[test_idx[-1]]

            # Purge: remove training samples whose eval_time overlaps test
            purge_mask = eval_times >= test_start
            # Embargo: remove samples too close after test
            embargo_mask = pred_times <= (test_end + embargo_gap)

            train_idx = np.concatenate([
                splits[j] for j in range(self.n_splits) if j != i
            ])
            # Apply purge: keep only train samples not overlapping test
            train_idx = train_idx[
                ~(purge_mask[train_idx] & ~embargo_mask[train_idx])
            ]
            yield train_idx, test_idx
