"""
Performance analytics for backtests and live trading.

compute_metrics: Sharpe, Sortino, Calmar, max drawdown, fill quality.
plot_pnl:        equity curve via matplotlib.
fill_report:     per-fill DataFrame with slippage analysis.
"""

from __future__ import annotations

from typing import List, Optional, Sequence
import numpy as np


def compute_metrics(equity: Sequence[float],
                    ann_factor: float = 1.0,
                    risk_free: float  = 0.0) -> dict:
    """
    Compute strategy performance metrics from an equity curve.

    equity     : portfolio value at each bar/tick (one value per period)
    ann_factor : periods per year for annualisation.
                 Default 1.0 = no annualisation (reports per-bar Sharpe).
                 Pass 252 for daily bars, 252*390 for 1-minute bars, etc.
    risk_free  : per-period risk-free rate (default 0).

    Returns dict: sharpe, sortino, calmar, max_drawdown, total_return,
                  profit_factor, volatility, n_bars.

    Note on Sharpe: applying sqrt(252) to tick-level data inflates the ratio
    by ~16× and produces numbers that are not comparable to published daily
    Sharpe ratios.  Use ann_factor=1.0 (default) and label results as
    "per-bar Sharpe" in any report.
    """
    eq = np.asarray(equity, dtype=float)
    if len(eq) < 2:
        return {}

    rets = np.diff(eq) / np.where(eq[:-1] != 0, eq[:-1], 1.0)
    if rets.std() == 0:
        return {"sharpe": 0.0, "sortino": 0.0, "calmar": 0.0,
                "max_drawdown": 0.0, "total_return": 0.0,
                "profit_factor": 0.0, "volatility": 0.0, "n_bars": len(rets)}

    mean_ret  = rets.mean()
    std_ret   = rets.std(ddof=1)
    rf_per_bar = risk_free / ann_factor if ann_factor > 0 else 0.0
    downside  = rets[rets < rf_per_bar]
    down_std  = downside.std(ddof=1) if len(downside) > 1 else 1e-9

    sharpe  = (mean_ret - rf_per_bar) / std_ret  * np.sqrt(ann_factor)
    sortino = (mean_ret - rf_per_bar) / down_std * np.sqrt(ann_factor)

    # Max drawdown
    cum      = eq / eq[0]
    roll_max = np.maximum.accumulate(cum)
    dd       = (cum - roll_max) / roll_max
    max_dd   = dd.min()

    # Calmar
    total_ret = (eq[-1] - eq[0]) / eq[0]
    calmar    = total_ret / abs(max_dd) if max_dd != 0 else 0.0

    # Profit factor
    wins  = rets[rets > 0].sum()
    losses = abs(rets[rets < 0].sum())
    pf    = wins / losses if losses > 0 else float('inf')

    return {
        "sharpe":       round(sharpe, 3),
        "sortino":      round(sortino, 3),
        "calmar":       round(calmar, 3),
        "max_drawdown": round(float(max_dd), 4),
        "total_return": round(float(total_ret), 4),
        "profit_factor":round(float(pf), 3),
        "volatility":   round(float(std_ret * np.sqrt(ann_factor)), 4),
        "n_bars":       len(rets),
    }


def fill_report(fills: List) -> "pd.DataFrame":
    """
    fills: list of FillEvent objects.
    Returns DataFrame with columns: ts_ns, symbol, side, qty, price, slippage_bps.
    Requires pandas.
    """
    try:
        import pandas as pd
    except ImportError:
        raise ImportError("pandas required for fill_report")

    rows = []
    for f in fills:
        rows.append({
            "ts_ns":    f.ts_ns,
            "symbol":   f.symbol,
            "side":     f.side,
            "qty":      f.qty,
            "price":    f.price,
            "is_maker": getattr(f, "is_maker", False),
        })

    if not rows:
        return pd.DataFrame()

    df = pd.DataFrame(rows)
    df["ts"] = pd.to_datetime(df["ts_ns"], unit="ns", utc=True)
    return df


def max_drawdown(equity: Sequence[float]) -> float:
    eq = np.asarray(equity, dtype=float)
    if len(eq) < 2:
        return 0.0
    roll_max = np.maximum.accumulate(eq)
    dd = (eq - roll_max) / roll_max
    return float(dd.min())


def plot_pnl(equity: Sequence[float], timestamps: Optional[Sequence] = None,
             title: str = "Equity Curve", save_path: Optional[str] = None,
             benchmark: Optional[Sequence[float]] = None,
             benchmark_label: str = "Benchmark"):
    """
    Plot equity curve + optional drawdown panel.

    benchmark : optional price series scaled to same start as equity (e.g. SPY closes).
    """
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        raise ImportError("matplotlib required for plot_pnl")

    fig, axes = plt.subplots(2, 1, figsize=(12, 7), sharex=True)

    eq = np.asarray(equity, dtype=float)
    x  = timestamps if timestamps is not None else np.arange(len(eq))

    # Equity curve
    axes[0].plot(x, eq, lw=1.2, color='steelblue', label='Strategy')
    if benchmark is not None and len(benchmark) > 1:
        bm  = np.asarray(benchmark, dtype=float)
        bm  = bm / bm[0] * eq[0]
        bx  = np.linspace(x[0], x[-1], len(bm)) if timestamps is None else \
              np.linspace(float(x[0]), float(x[-1]), len(bm))
        axes[0].plot(bx, bm, lw=1.0, color='orange', alpha=0.75, label=benchmark_label)
        axes[0].legend(fontsize=8)
    axes[0].set_title(title)
    axes[0].set_ylabel("Portfolio Value")
    axes[0].grid(alpha=0.3)

    # Drawdown
    roll_max = np.maximum.accumulate(eq)
    dd = (eq - roll_max) / np.where(roll_max != 0, roll_max, 1.0) * 100
    axes[1].fill_between(x, dd, 0, alpha=0.5, color='crimson')
    axes[1].set_ylabel("Drawdown (%)")
    axes[1].set_xlabel("Time")
    axes[1].grid(alpha=0.3)

    plt.tight_layout()
    if save_path:
        plt.savefig(save_path, dpi=150)
    else:
        plt.show()
    return fig


def plot_depth(lob, n: int = 10, title: str = "Order Book Depth"):
    """Visualize LOB depth. lob must have bid_depth/ask_depth methods."""
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        raise ImportError("matplotlib required for plot_depth")

    bids = lob.bid_depth(n)
    asks = lob.ask_depth(n)

    bid_prices = [b.price for b in bids]
    bid_qtys   = [b.qty   for b in bids]
    ask_prices = [a.price for a in asks]
    ask_qtys   = [a.qty   for a in asks]

    fig, ax = plt.subplots(figsize=(8, 4))
    ax.barh(bid_prices, bid_qtys,  height=0.005, color='green', alpha=0.7, label='Bids')
    ax.barh(ask_prices, [-q for q in ask_qtys], height=0.005, color='red', alpha=0.7, label='Asks')
    ax.axvline(0, color='black', lw=0.8)
    ax.set_xlabel("Quantity")
    ax.set_ylabel("Price")
    ax.set_title(title)
    ax.legend()
    plt.tight_layout()
    plt.show()
    return fig
