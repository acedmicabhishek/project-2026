"""
QuantSim — Full-platform quant developer simulator.

C++ core: matching engine, LOB, risk, feed handlers.
Python layer: strategy framework, backtester, analytics, dashboard.
"""

try:
    import quantsim_core as _core
    from quantsim_core import (
        Side, OrdType, OrdStatus,
        Order, Fill, BookSnapshot, DepthLevel,
        LimitOrderBook, MatchingEngine, MatchResult,
        KillSwitch, PreTradeRisk, RiskLimits, RiskResult,
        TradingMetrics,
        TickEvent, CsvTickReplay, SyntheticFeed, SyntheticParams,
    )
    CORE_AVAILABLE = True
except ImportError:
    CORE_AVAILABLE = False

from .strategy   import Strategy, BookUpdate, FillEvent, OrderRef
from .backtester import BacktestEngine, BacktestResult, WalkForwardTest, PurgedKFold
from .analytics  import compute_metrics, plot_pnl, fill_report
from .data.synthetic import GBMFeed, OUSpreadFeed

__version__ = "0.1.0"
__all__ = [
    "Strategy", "BookUpdate", "FillEvent", "OrderRef",
    "BacktestEngine", "BacktestResult", "WalkForwardTest", "PurgedKFold",
    "compute_metrics", "plot_pnl", "fill_report",
    "GBMFeed", "OUSpreadFeed",
    "CORE_AVAILABLE",
]
