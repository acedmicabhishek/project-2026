"""
QuantSim Interactive Runner — live terminal dashboard

Usage:
    python3 run_sim.py                          # market maker, 10000 ticks
    python3 run_sim.py --strategy stat_arb
    python3 run_sim.py --strategy twap
    python3 run_sim.py --strategy mm --ticks 20000 --speed 0.0005
    python3 run_sim.py --demo                   # show all 3 strategies sequentially
"""

import argparse
import sys
import os
import time
import threading
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'python'))

import numpy as np
from rich.console     import Console
from rich.layout      import Layout
from rich.live        import Live
from rich.table       import Table
from rich.panel       import Panel
from rich.text        import Text
from rich.progress    import Progress, BarColumn, TextColumn, TimeElapsedColumn
from rich.columns     import Columns
from rich             import box
from rich.align       import Align
from rich.rule        import Rule
from rich.style       import Style

from quantsim.strategy    import Strategy, BookUpdate, FillEvent
from quantsim.backtester  import BacktestEngine
from quantsim.analytics   import compute_metrics, max_drawdown
from quantsim.data.synthetic import GBMFeed, OUSpreadFeed


# ── Shared state updated by strategy, read by dashboard ─────────────────────

class SimState:
    def __init__(self):
        self.snap:     BookUpdate         = None
        self.fills:    deque              = deque(maxlen=12)
        self.equity:   list               = []
        self.position: dict               = {}
        self.pnl:      float              = 0.0
        self.tick_num: int                = 0
        self.total_ticks: int             = 0
        self.strategy_name: str           = ""
        self.metrics:  dict               = {}
        self.orders_sent: int             = 0
        self.orders_filled: int           = 0
        self.kill_armed: bool             = False
        self.done:    bool                = False
        self.initial_cash: float          = 500_000.0
        self.mids:    deque               = deque(maxlen=80)  # for sparkline
        self._lock = threading.Lock()

    def update_snap(self, s): self.snap = s
    def add_fill(self, f):
        with self._lock:
            self.fills.append(f)
            self.orders_filled += 1
    def set_equity(self, eq, pos):
        with self._lock:
            self.equity = eq[:]
            self.position = dict(pos)
            self.pnl = (eq[-1] - self.initial_cash) if eq else 0.0
            if eq:
                self.mids.append(eq[-1])
    def update_metrics(self):
        if len(self.equity) > 10:
            self.metrics = compute_metrics(self.equity)


# ── Instrumented Strategy Wrappers ───────────────────────────────────────────

class InstrumentedMM(Strategy):
    def __init__(self, symbol, gamma=0.1, sigma=0.12, order_qty=100,
                 reprice_threshold=0.005, state: SimState = None):
        super().__init__()
        import math
        self.math     = math
        self.symbol   = symbol
        self.gamma    = gamma
        self.sigma    = sigma
        self.order_qty= order_qty
        self.reprice_threshold = reprice_threshold
        self.state    = state
        self._inv     = 0
        self._last_mid= 0.0
        self._bid_id  = 0
        self._ask_id  = 0
        self._tick_n  = 0
        self._total   = state.total_ticks if state else 1000

    def reservation_price(self, mid):
        q = self._inv
        T = max(0.01, 1.0 - self._tick_n / self._total)
        return mid - q * self.gamma * self.sigma**2 * T

    def optimal_spread(self):
        T = max(0.01, 1.0 - self._tick_n / self._total)
        return max(0.005,
            self.gamma * self.sigma**2 * T / 2.0 +
            self.math.log(1.0 + self.gamma / 50.0) / self.gamma)

    def on_book_update(self, u: BookUpdate):
        self._tick_n += 1
        if self.state: self.state.tick_num = self._tick_n
        mid = u.mid
        if self.state: self.state.update_snap(u)
        if abs(self._inv) >= 1000:
            self.cancel_all(); self._bid_id = self._ask_id = 0; return
        r = self.reservation_price(mid)
        s = self.optimal_spread()
        reprice = (self._last_mid == 0 or
                   abs(mid - self._last_mid) > self.reprice_threshold)
        if reprice:
            if self._bid_id: self.cancel_order(self._bid_id); self._bid_id = 0
            if self._ask_id: self.cancel_order(self._ask_id); self._ask_id = 0
            self._bid_id = self.send_order(self.symbol,'B',self.order_qty,
                                            round(r-s,4),'LIMIT')
            self._ask_id = self.send_order(self.symbol,'S',self.order_qty,
                                            round(r+s,4),'LIMIT')
            if self.state: self.state.orders_sent += 2
            self._last_mid = mid

    def on_fill(self, f: FillEvent):
        super().on_fill(f)
        self._inv += f.qty if f.side=='B' else -f.qty
        if self.state: self.state.add_fill(f)
        if f.order_id == self._bid_id: self._bid_id = 0
        elif f.order_id == self._ask_id: self._ask_id = 0


class InstrumentedStatArb(Strategy):
    def __init__(self, sym_a, sym_b, entry_z=2.0, window=60,
                 order_qty=100, state: SimState = None):
        super().__init__()
        import math
        self.math     = math
        self.sym_a    = sym_a
        self.sym_b    = sym_b
        self.entry_z  = entry_z
        self.exit_z   = 0.3
        self.stop_z   = 4.0
        self.order_qty= order_qty
        self.window   = window
        self.state    = state
        self._buf: deque = deque(maxlen=window)
        self._mids: dict = {}
        self._mode  = 'flat'
        self._tick_n= 0

    def _zscore(self, x):
        self._buf.append(x)
        if len(self._buf) < self.window: return 0.0
        a = list(self._buf)
        m = sum(a)/len(a)
        s = self.math.sqrt(sum((v-m)**2 for v in a)/len(a)) or 1e-9
        return (x - m) / s

    def on_book_update(self, u: BookUpdate):
        self._tick_n += 1
        self._mids[u.symbol] = u.mid
        if self.state: self.state.tick_num = self._tick_n; self.state.update_snap(u)
        if self.sym_a not in self._mids or self.sym_b not in self._mids: return
        spread = self._mids[self.sym_a] - self._mids[self.sym_b]
        z = self._zscore(spread)
        if len(self._buf) < self.window: return
        if self._mode == 'flat':
            if z > self.entry_z:
                self.send_order(self.sym_a,'S',self.order_qty,self._mids[self.sym_a],'MARKET')
                self.send_order(self.sym_b,'B',self.order_qty,self._mids[self.sym_b],'MARKET')
                self._mode = 'short_spread'
                if self.state: self.state.orders_sent += 2
            elif z < -self.entry_z:
                self.send_order(self.sym_a,'B',self.order_qty,self._mids[self.sym_a],'MARKET')
                self.send_order(self.sym_b,'S',self.order_qty,self._mids[self.sym_b],'MARKET')
                self._mode = 'long_spread'
                if self.state: self.state.orders_sent += 2
        elif self._mode == 'short_spread':
            if abs(z) < self.exit_z or z < -self.stop_z:
                self.send_order(self.sym_a,'B',self.order_qty,self._mids[self.sym_a],'MARKET')
                self.send_order(self.sym_b,'S',self.order_qty,self._mids[self.sym_b],'MARKET')
                self._mode = 'flat'
        elif self._mode == 'long_spread':
            if abs(z) < self.exit_z or z > self.stop_z:
                self.send_order(self.sym_a,'S',self.order_qty,self._mids[self.sym_a],'MARKET')
                self.send_order(self.sym_b,'B',self.order_qty,self._mids[self.sym_b],'MARKET')
                self._mode = 'flat'

    def on_fill(self, f: FillEvent):
        super().on_fill(f)
        if self.state: self.state.add_fill(f)


# ── Dashboard Renderer ────────────────────────────────────────────────────────

def render(state: SimState, strategy_name: str) -> Layout:
    layout = Layout()
    layout.split_column(
        Layout(name="header", size=3),
        Layout(name="middle"),
        Layout(name="fills",  size=15),
        Layout(name="footer", size=3),
    )
    layout["middle"].split_row(
        Layout(name="book",    ratio=2),
        Layout(name="metrics", ratio=2),
        Layout(name="equity",  ratio=3),
    )

    pct = (state.tick_num / max(state.total_ticks, 1)) * 100
    bar = "█" * int(pct / 2) + "░" * (50 - int(pct / 2))
    pnl_color = "bold green" if state.pnl >= 0 else "bold red"
    ks_color  = "bold red"   if state.kill_armed else "bold green"

    # ── Header
    snap = state.snap
    if snap:
        hdr = (f"  [bold cyan]QuantSim[/] │ [yellow]{strategy_name}[/]"
               f" │ [white]{snap.symbol}[/]"
               f"  Bid:[green]{snap.bid:.4f}[/]  Ask:[red]{snap.ask:.4f}[/]"
               f"  Mid:[white]{snap.mid:.4f}[/]  Spread:[yellow]{snap.spread:.4f}[/]"
               f"  │  PnL:[{pnl_color}]${state.pnl:+,.2f}[/]"
               f"  │  Kill:[{ks_color}]{'ARMED' if state.kill_armed else 'OK'}[/]")
    else:
        hdr = f"  [bold cyan]QuantSim[/] │ [yellow]{strategy_name}[/] │ [dim]Initializing...[/]"
    layout["header"].update(Panel(hdr, box=box.ROUNDED, style="on grey7"))

    # ── Book depth
    book_tbl = Table(box=box.SIMPLE, show_header=True, expand=True, style="on grey7")
    book_tbl.add_column("Bid Qty",  style="green",  justify="right", min_width=8)
    book_tbl.add_column("Bid",      style="bold green", justify="right")
    book_tbl.add_column("Ask",      style="bold red",   justify="right")
    book_tbl.add_column("Ask Qty",  style="red",    justify="left",  min_width=8)
    if snap:
        for i in range(5):
            b_spread = 0.01 * (i + 1)
            bq = max(50, 500 - i*70 + (hash(str(state.tick_num)+str(i)) % 100 - 50))
            aq = max(50, 500 - i*70 + (hash(str(state.tick_num+1)+str(i)) % 100 - 50))
            book_tbl.add_row(
                str(bq),
                f"{snap.bid - b_spread*i:.4f}",
                f"{snap.ask + b_spread*i:.4f}",
                str(aq),
            )
    else:
        book_tbl.add_row("-", "-", "-", "-")
    layout["book"].update(Panel(book_tbl, title="[bold]Order Book[/]",
                                 box=box.ROUNDED, style="on grey7"))

    # ── Metrics
    m = state.metrics
    pos_str = ", ".join(f"{k}:{v:+d}" for k,v in state.position.items()) or "flat"
    met_tbl = Table(box=box.SIMPLE, show_header=False, expand=True, style="on grey7")
    met_tbl.add_column("Key",   style="bold white", min_width=14)
    met_tbl.add_column("Value", style="cyan")
    rows = [
        ("Sharpe",    f"{m.get('sharpe', 0.0):.3f}"),
        ("Sortino",   f"{m.get('sortino', 0.0):.3f}"),
        ("Max DD",    f"{m.get('max_drawdown', 0.0)*100:.2f}%"),
        ("Profit Fac",f"{m.get('profit_factor', 0.0):.3f}" if m.get('profit_factor',0) < 1000 else "∞"),
        ("Volatility",f"{m.get('volatility', 0.0)*100:.3f}%"),
        ("Fills",     str(state.orders_filled)),
        ("Orders Sent",str(state.orders_sent)),
        ("Position",  pos_str),
        ("Progress",  f"{state.tick_num}/{state.total_ticks}  {pct:.1f}%"),
    ]
    for k,v in rows: met_tbl.add_row(k, v)
    layout["metrics"].update(Panel(met_tbl, title="[bold]Metrics[/]",
                                    box=box.ROUNDED, style="on grey7"))

    # ── Equity sparkline (ASCII)
    eq = list(state.equity)[-60:] if state.equity else []
    spark = ""
    if len(eq) > 2:
        mn, mx = min(eq), max(eq)
        rng = mx - mn or 1.0
        bars = " ▁▂▃▄▅▆▇█"
        spark = "".join(bars[max(0, min(8, int((v-mn)/rng*8)))] for v in eq)
    eq_panel_text = (
        f"[green]{spark}[/]\n\n"
        f"  Initial: [white]${state.initial_cash:,.2f}[/]\n"
        f"  Current: [{'green' if state.pnl>=0 else 'red'}]${(state.initial_cash+state.pnl):,.2f}[/]\n"
        f"  PnL:     [bold {'green' if state.pnl>=0 else 'red'}]${state.pnl:+,.2f}[/]\n"
        f"  Return:  [{'green' if state.pnl>=0 else 'red'}]{state.pnl/state.initial_cash*100:+.3f}%[/]"
    )
    layout["equity"].update(Panel(eq_panel_text, title="[bold]Equity Curve[/]",
                                   box=box.ROUNDED, style="on grey7"))

    # ── Fill log
    fill_tbl = Table(box=box.SIMPLE, show_header=True, expand=True, style="on grey7")
    fill_tbl.add_column("Order ID",  justify="right",  min_width=9)
    fill_tbl.add_column("Symbol",    justify="center",  min_width=6)
    fill_tbl.add_column("Side",      justify="center",  min_width=5)
    fill_tbl.add_column("Qty",       justify="right",   min_width=6)
    fill_tbl.add_column("Price",     justify="right",   min_width=10)
    fill_tbl.add_column("Maker",     justify="center",  min_width=6)
    fills = list(state.fills)
    for f in reversed(fills[-8:]):
        side_sty = "green" if f.side in ('B','BUY') else "red"
        fill_tbl.add_row(
            str(f.order_id),
            f.symbol,
            f"[{side_sty}]{f.side}[/]",
            str(f.qty),
            f"{f.price:.4f}",
            "✓" if getattr(f,'is_maker',False) else "✗",
        )
    layout["fills"].update(Panel(fill_tbl, title=f"[bold]Recent Fills[/]  (total: {state.orders_filled})",
                                  box=box.ROUNDED, style="on grey7"))

    # ── Footer progress bar
    done_pct_bar = "█" * int(pct/2) + "░" * (50 - int(pct/2))
    footer_txt = f"  Progress: [cyan]{done_pct_bar}[/] {pct:.1f}%  │  Tick {state.tick_num}/{state.total_ticks}"
    if state.done:
        footer_txt = f"  [bold green]✓ SIMULATION COMPLETE[/]  │  Final PnL: [bold {'green' if state.pnl>=0 else 'red'}]${state.pnl:+,.2f}[/]"
    layout["footer"].update(Panel(footer_txt, box=box.ROUNDED, style="on grey7"))

    return layout


# ── Run a simulation with live dashboard ─────────────────────────────────────

def run_live(strategy_name: str, ticks: int, sigma: float = 0.0008,
             speed_delay: float = 0.0, seed: int = 42):
    """
    speed_delay: seconds between ticks (0 = max speed + dashboard refresh only)
    """
    console = Console()
    state   = SimState()
    state.total_ticks    = ticks
    state.strategy_name  = strategy_name
    state.initial_cash   = 500_000.0

    # Build ticks
    if strategy_name in ('mm', 'market_maker'):
        feed   = GBMFeed("AAPL", initial_price=150.0, sigma=sigma, seed=seed)
        data   = feed.generate(ticks)
        strat  = InstrumentedMM("AAPL", sigma=sigma, state=state)

    elif strategy_name in ('stat_arb', 'sa'):
        feed   = OUSpreadFeed("SPY","QQQ", initial_price=450.0,
                               theta=0.08, sigma_spread=0.05,
                               sigma_price=0.001, seed=seed)
        ta, tb = feed.generate(ticks)
        data   = sorted(ta + tb, key=lambda t: t.ts_ns)
        state.total_ticks = len(data)
        strat  = InstrumentedStatArb("SPY","QQQ", entry_z=2.0,
                                      window=60, state=state)

    elif strategy_name == 'twap':
        feed   = GBMFeed("TSLA", initial_price=200.0, sigma=sigma, seed=seed)
        data   = feed.generate(ticks)
        from examples.twap import TWAPStrategy
        strat  = TWAPStrategy("TSLA", total_qty=5000, side='B',
                               n_slices=20,
                               ticks_per_slice=max(1, ticks//20))
    else:
        console.print(f"[red]Unknown strategy: {strategy_name}[/]")
        return

    engine = BacktestEngine(initial_cash=state.initial_cash, timer_interval_ns=0)
    strat._attach(engine)

    # Override engine callbacks to feed state
    orig_fill = engine._on_cpp_fill if hasattr(engine,'_on_cpp_fill') else None

    # Run in thread; dashboard in main
    result_box = [None]
    REFRESH_EVERY = max(1, ticks // 200)  # update display every N ticks

    def sim_thread():
        try:
            strat.on_start()
            engine._strategy = strat
            engine._fills.clear()
            engine._equity.clear()
            engine._positions.clear()
            engine._cash = state.initial_cash
            engine._mids = {}
            engine._last_snap = None
            engine._order_seq  = 0
            engine._pending_orders = {}

            for i, tick in enumerate(data):
                ts_ns  = getattr(tick,'ts_ns',  i*1_000_000)
                symbol = getattr(tick,'symbol', '')
                bid    = getattr(tick,'bid',    0.0)
                ask    = getattr(tick,'ask',    0.0)
                bsz    = getattr(tick,'size',   100)
                if bid<=0 or ask<=0 or ask<bid: continue

                mid = (bid+ask)/2
                engine._mids[symbol] = mid
                engine._bids[symbol] = bid
                engine._asks[symbol] = ask
                engine._fill_resting(symbol, bid, ask, ts_ns)
                engine._process_pending_orders(ts_ns)

                upd = BookUpdate(symbol=symbol,bid=bid,ask=ask,
                                  bid_sz=bsz,ask_sz=bsz,ts_ns=ts_ns)
                engine._last_snap = upd
                strat.on_book_update(upd)

                eq = engine._mark_equity()
                engine._equity.append(eq)
                engine._ts_log.append(ts_ns)

                if i % REFRESH_EVERY == 0:
                    state.set_equity(engine._equity, engine._positions)
                    state.update_metrics()
                    if speed_delay > 0:
                        time.sleep(speed_delay)

            strat.on_stop()
            state.set_equity(engine._equity, engine._positions)
            state.update_metrics()
        finally:
            state.done = True
            result_box[0] = engine._equity

    t = threading.Thread(target=sim_thread, daemon=True)
    t.start()

    with Live(render(state, strategy_name), console=console,
              refresh_per_second=10, screen=True) as live:
        while not state.done or not t.is_alive() == False:
            live.update(render(state, strategy_name))
            time.sleep(0.1)
            if state.done and not t.is_alive():
                # Show final state for 3 seconds
                live.update(render(state, strategy_name))
                time.sleep(3)
                break
        t.join(timeout=2)

    # Final summary
    m = state.metrics
    console.print()
    console.rule(f"[bold cyan]QuantSim — {strategy_name.upper()} Results[/]")
    console.print(f"  Ticks processed : [white]{state.tick_num:,}[/]")
    console.print(f"  Total fills     : [white]{state.orders_filled:,}[/]")
    console.print(f"  Final PnL       : [bold {'green' if state.pnl>=0 else 'red'}]${state.pnl:+,.2f}[/]")
    console.print(f"  Sharpe Ratio    : [cyan]{m.get('sharpe',0):.3f}[/]")
    console.print(f"  Max Drawdown    : [yellow]{m.get('max_drawdown',0)*100:.2f}%[/]")
    _pf = m.get('profit_factor', 0)
    _pf_str = f"{_pf:.3f}" if _pf < 1000 else "∞"
    console.print(f"  Profit Factor   : [cyan]{_pf_str}[/]")
    console.print(f"  Open positions  : [white]{state.position}[/]")
    console.rule()


def run_demo():
    """Run all 3 strategies sequentially."""
    console = Console()
    strategies = [
        ("mm",       5000, 0.0008),
        ("stat_arb", 3000, 0.0008),
        ("twap",     2000, 0.001),
    ]
    for name, ticks, sigma in strategies:
        console.print(f"\n[bold cyan]▶ Running {name.upper()} ({ticks} ticks)...[/]")
        time.sleep(0.5)
        run_live(name, ticks, sigma=sigma)
        console.print()
        time.sleep(1)


# ── CLI ───────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="QuantSim Interactive Runner")
    parser.add_argument("--strategy", "-s", default="mm",
                        choices=["mm","market_maker","stat_arb","sa","twap"],
                        help="Strategy to run")
    parser.add_argument("--ticks",    "-t", type=int,   default=10_000)
    parser.add_argument("--sigma",         type=float,  default=0.0008)
    parser.add_argument("--speed",         type=float,  default=0.0,
                        help="Seconds between ticks (0=max speed)")
    parser.add_argument("--seed",          type=int,    default=42)
    parser.add_argument("--demo",          action="store_true",
                        help="Run all 3 strategies sequentially")
    args = parser.parse_args()

    if args.demo:
        run_demo()
    else:
        run_live(args.strategy, args.ticks, sigma=args.sigma,
                 speed_delay=args.speed, seed=args.seed)
