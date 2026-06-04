"""
Terminal dashboard using `rich`.

Run standalone:
    python -m quantsim.dashboard --strategy market_maker --ticks 5000

Or embed in a live sim:
    dash = Dashboard()
    dash.update(book_update, fills, metrics)
    dash.render()
"""

from __future__ import annotations

import time
from typing import List, Optional, Dict

try:
    from rich.console     import Console
    from rich.table       import Table
    from rich.panel       import Panel
    from rich.layout      import Layout
    from rich.live        import Live
    from rich.text        import Text
    from rich             import box
    RICH_AVAILABLE = True
except ImportError:
    RICH_AVAILABLE = False

from .strategy import BookUpdate, FillEvent


class Dashboard:
    """Real-time terminal dashboard for a running strategy."""

    def __init__(self, symbol: str = ""):
        if not RICH_AVAILABLE:
            raise ImportError("pip install rich")
        self.symbol    = symbol
        self.console   = Console()
        self._snap:    Optional[BookUpdate] = None
        self._fills:   List[FillEvent]      = []
        self._metrics: Dict[str, object]    = {}
        self._position = 0
        self._pnl      = 0.0
        self._kill_switch_armed = False
        self._orders_sent = 0
        self._latency_p99 = 0.0

    def update(self, snap: Optional[BookUpdate] = None,
               fills: Optional[List[FillEvent]] = None,
               metrics: Optional[dict] = None,
               position: int = 0, pnl: float = 0.0,
               kill_switch_armed: bool = False):
        if snap:    self._snap = snap
        if fills:   self._fills.extend(fills); self._fills = self._fills[-20:]
        if metrics: self._metrics.update(metrics)
        self._position         = position
        self._pnl              = pnl
        self._kill_switch_armed= kill_switch_armed

    def _make_header(self) -> Panel:
        ks_color = "bold red" if self._kill_switch_armed else "bold green"
        ks_text  = "ARMED" if self._kill_switch_armed else "OK"
        snap = self._snap

        if snap:
            header = (
                f"  [bold]{snap.symbol or self.symbol}[/]  "
                f"  Bid: [green]{snap.bid:.4f}[/]  Ask: [red]{snap.ask:.4f}[/]"
                f"  Spread: [yellow]{snap.spread:.4f}[/]  Mid: {snap.mid:.4f}"
                f"  |  Pos: [cyan]{self._position:+d}[/]"
                f"  PnL: {'[green]' if self._pnl >= 0 else '[red]'}{self._pnl:+.2f}[/]"
                f"  |  Kill: [{ks_color}]{ks_text}[/]"
            )
        else:
            header = f"[dim]No data yet[/]  Kill: [{ks_color}]{ks_text}[/]"

        return Panel(header, title="[bold]QuantSim Dashboard[/]", box=box.ROUNDED)

    def _make_depth_table(self, n: int = 5) -> Table:
        table = Table(title="Order Book", box=box.SIMPLE, show_header=True)
        table.add_column("Bid Qty",  style="green", justify="right")
        table.add_column("Bid",      style="green", justify="right")
        table.add_column("Ask",      style="red",   justify="right")
        table.add_column("Ask Qty",  style="red",   justify="left")

        if self._snap:
            # Synthetic depth display from best bid/ask
            for i in range(n):
                tick = 0.01 * (i + 1)
                bq = max(100, 500 - i * 80)
                aq = max(100, 500 - i * 80)
                bp = self._snap.bid - tick * i
                ap = self._snap.ask + tick * i
                table.add_row(str(bq), f"{bp:.4f}", f"{ap:.4f}", str(aq))
        else:
            table.add_row("-", "-", "-", "-")
        return table

    def _make_fill_table(self) -> Table:
        table = Table(title="Recent Fills", box=box.SIMPLE, show_header=True)
        table.add_column("Time (ns)",  style="dim",   justify="right")
        table.add_column("Symbol",     justify="left")
        table.add_column("Side",       justify="center")
        table.add_column("Qty",        justify="right")
        table.add_column("Price",      justify="right")

        recent = self._fills[-10:]
        for f in reversed(recent):
            side_style = "green" if f.side in ('B', 'BUY') else "red"
            table.add_row(
                str(f.ts_ns),
                f.symbol,
                f"[{side_style}]{f.side}[/]",
                str(f.qty),
                f"{f.price:.4f}",
            )
        return table

    def _make_metrics_table(self) -> Table:
        table = Table(title="Metrics", box=box.SIMPLE, show_header=False)
        table.add_column("Key",   style="bold")
        table.add_column("Value", style="cyan")

        m = self._metrics
        rows = [
            ("Sharpe",      f"{m.get('sharpe', 0):.3f}"),
            ("Max DD",      f"{m.get('max_drawdown', 0)*100:.2f}%"),
            ("Fills",       str(len(self._fills))),
            ("p99 lat (µs)",f"{self._latency_p99:.1f}"),
        ]
        for k, v in rows:
            table.add_row(k, v)
        return table

    def render(self):
        """Print one frame to terminal."""
        self.console.clear()
        self.console.print(self._make_header())
        self.console.print(self._make_depth_table())
        self.console.print(self._make_fill_table())
        self.console.print(self._make_metrics_table())

    def live_run(self, engine_fn, update_hz: float = 4.0):
        """
        Run engine_fn in a loop, refreshing dashboard at update_hz.
        engine_fn(dashboard) should call dashboard.update() then return.
        """
        interval = 1.0 / update_hz
        with Live(refresh_per_second=update_hz, console=self.console) as live:
            while True:
                engine_fn(self)
                layout = Layout()
                layout.split_column(
                    Layout(self._make_header(),       name="header", size=3),
                    Layout(self._make_depth_table(),  name="depth"),
                    Layout(self._make_fill_table(),   name="fills"),
                    Layout(self._make_metrics_table(),name="metrics", size=8),
                )
                live.update(layout)
                time.sleep(interval)
