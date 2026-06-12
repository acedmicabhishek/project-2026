"""
Tear-sheet generator and trade exporter.

Usage:
    from quantsim.research.report import TearSheet, fetch_benchmark

    bm = fetch_benchmark("SPY", period="1y", interval="1d")
    ts = TearSheet(result, symbol="AAPL", source="yfinance", benchmark_equity=bm)
    ts.export_trades("trades.csv")
    ts.save("tearsheet.png")
"""

from __future__ import annotations

import csv
from typing import Optional, Sequence

import numpy as np

from quantsim.analytics import compute_metrics


# ── Public API ────────────────────────────────────────────────────────────────

class TearSheet:
    """
    Single-strategy tear-sheet: equity vs benchmark, drawdown, returns histogram,
    and a metrics table. Renders to PNG (dark theme matching the GUI).
    """

    def __init__(self, result, symbol: str = "",
                 source: str = "synthetic",
                 benchmark_equity: Optional[Sequence[float]] = None,
                 benchmark_label: str = "SPY"):
        self.result           = result
        self.symbol           = symbol
        self.source           = source
        self.benchmark_equity = benchmark_equity
        self.benchmark_label  = benchmark_label

    # ── Trade export ──────────────────────────────────────────────────────────

    def export_trades(self, path: str = "trades.csv") -> str:
        """Write all fills to CSV. Returns the file path."""
        fills = self.result.fills
        with open(path, "w", newline="") as f:
            writer = csv.DictWriter(
                f, fieldnames=["ts_ns", "symbol", "side", "qty", "price", "is_maker"])
            writer.writeheader()
            for fill in fills:
                writer.writerow({
                    "ts_ns":    fill.ts_ns,
                    "symbol":   fill.symbol,
                    "side":     fill.side,
                    "qty":      fill.qty,
                    "price":    fill.price,
                    "is_maker": getattr(fill, "is_maker", False),
                })
        return path

    # ── PNG render ────────────────────────────────────────────────────────────

    def save(self, path: str = "tearsheet.png",
             ann_factor: float = 252.0) -> str:
        """Render tear-sheet to PNG. Returns the file path."""
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.gridspec as gridspec

        eq = np.asarray(self.result.equity, dtype=float)
        m  = compute_metrics(eq, ann_factor=ann_factor)

        BG   = "#0f0f0f"
        CELL = "#1c1c1e"
        CELL2= "#2c2c2e"
        GRID = "#48484a"
        FG   = "#ebebf5"
        BLUE = "#0a84ff"
        RED  = "#ff453a"
        GRN  = "#30d158"
        ORG  = "#ff9f0a"

        fig = plt.figure(figsize=(14, 10), facecolor=BG)
        fig.suptitle(
            f"QuantSim — {self.symbol}  ({self.source})",
            color=FG, fontsize=13, y=0.98)

        gs = gridspec.GridSpec(
            3, 2, figure=fig,
            hspace=0.48, wspace=0.30,
            left=0.07, right=0.97, top=0.93, bottom=0.06)

        # ── Equity curve ──────────────────────────────────────────────────
        ax_eq = fig.add_subplot(gs[0, :])
        x = np.arange(len(eq))
        ax_eq.plot(x, eq, lw=1.3, color=BLUE, label="Strategy")

        if self.benchmark_equity is not None and len(self.benchmark_equity) > 1:
            bm  = np.asarray(self.benchmark_equity, dtype=float)
            bm  = bm / bm[0] * eq[0]          # scale to same start value
            bx  = np.linspace(0, len(eq) - 1, len(bm))
            ax_eq.plot(bx, bm, lw=1.0, color=ORG, alpha=0.75,
                       label=self.benchmark_label)

        _style_ax(ax_eq, "Equity Curve", "Portfolio Value", CELL, GRID, FG)
        ax_eq.legend(facecolor=CELL2, labelcolor=FG, fontsize=8, framealpha=0.9)

        # ── Drawdown ──────────────────────────────────────────────────────
        ax_dd = fig.add_subplot(gs[1, :])
        roll_max = np.maximum.accumulate(eq)
        dd = (eq - roll_max) / np.where(roll_max != 0, roll_max, 1.0) * 100
        ax_dd.fill_between(x, dd, 0, alpha=0.7, color=RED)
        ax_dd.plot(x, dd, lw=0.6, color=RED, alpha=0.5)
        _style_ax(ax_dd, "Drawdown (%)", "DD %", CELL, GRID, FG)

        # ── Returns distribution ──────────────────────────────────────────
        ax_hist = fig.add_subplot(gs[2, 0])
        rets = np.diff(eq) / np.where(eq[:-1] != 0, eq[:-1], 1.0) * 100
        ax_hist.hist(rets, bins=60, color=GRN, alpha=0.75, edgecolor="none")
        ax_hist.axvline(0, color=FG, lw=0.8, ls="--")
        ax_hist.axvline(float(np.mean(rets)), color=BLUE, lw=0.8, ls="--",
                        label=f"mean {np.mean(rets):.3f}%")
        _style_ax(ax_hist, "Returns Distribution (%)", "Count", CELL, GRID, FG)
        ax_hist.legend(facecolor=CELL2, labelcolor=FG, fontsize=7, framealpha=0.9)

        # ── Metrics table ─────────────────────────────────────────────────
        ax_tbl = fig.add_subplot(gs[2, 1])
        ax_tbl.axis("off")
        ax_tbl.set_facecolor(CELL)

        win_pct = _win_rate(self.result.fills)
        rows = [
            ("Total Return",  f"{m.get('total_return',0)*100:.2f}%"),
            ("Sharpe (ann)",  f"{m.get('sharpe',0):.3f}"),
            ("Sortino",       f"{m.get('sortino',0):.3f}"),
            ("Calmar",        f"{m.get('calmar',0):.3f}"),
            ("Max Drawdown",  f"{m.get('max_drawdown',0)*100:.2f}%"),
            ("Volatility",    f"{m.get('volatility',0)*100:.2f}%"),
            ("Profit Factor", f"{m.get('profit_factor',0):.3f}"),
            ("Win Rate",      f"{win_pct:.1f}%"),
            ("Total Fills",   str(len(self.result.fills))),
            ("Final PnL",     f"${self.result.final_pnl:,.0f}"),
        ]

        tbl = ax_tbl.table(
            cellText=rows,
            colLabels=["Metric", "Value"],
            cellLoc="left",
            loc="center",
        )
        tbl.auto_set_font_size(False)
        tbl.set_fontsize(9)
        tbl.scale(1.0, 1.45)

        for (r, c), cell in tbl.get_celld().items():
            cell.set_facecolor(CELL2 if r % 2 == 0 else CELL)
            cell.set_text_props(color=FG)
            cell.set_edgecolor(GRID)

        ax_tbl.set_title("Performance Metrics", color=FG, fontsize=10, pad=4)

        plt.savefig(path, dpi=150, facecolor=BG)
        plt.close(fig)
        return path


# ── Helpers ───────────────────────────────────────────────────────────────────

def _style_ax(ax, title: str, ylabel: str, bg: str, grid: str, fg: str):
    ax.set_title(title, color=fg, fontsize=10, pad=4)
    ax.set_ylabel(ylabel, color=fg, fontsize=8)
    ax.tick_params(colors=fg, labelsize=7)
    ax.set_facecolor(bg)
    ax.grid(alpha=0.25, color=grid)
    for sp in ax.spines.values():
        sp.set_color(grid)


def _win_rate(fills) -> float:
    """Rough win-rate: fraction of paired buy/sell fills where sell > buy."""
    buys  = [f for f in fills if f.side in ("B", "BUY")]
    sells = [f for f in fills if f.side in ("S", "SELL")]
    n = min(len(buys), len(sells))
    if n == 0:
        return 0.0
    wins = sum(1 for b, s in zip(buys[:n], sells[:n]) if s.price > b.price)
    return wins / n * 100.0


def fetch_benchmark(symbol: str = "SPY", period: str = "1y",
                    interval: str = "1d") -> list[float]:
    """Fetch closing prices for a benchmark symbol. Returns empty list on failure."""
    try:
        from quantsim.data.providers.yfinance_feed import YFinanceFeed
        ticks = YFinanceFeed().fetch(symbol, period=period, interval=interval)
        return [t.last for t in ticks if t.last and t.last > 0]
    except Exception:
        return []
