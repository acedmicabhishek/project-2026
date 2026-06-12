"""
QuantSim live trading layer.

Real-time market data + paper/live order routing. Built the way a real desk
splits it: a normalized feed handler, an order book it reconstructs locally, a
gateway that fills orders against that book, and an engine that drives strategies
and enforces risk.

Public API:
    BinanceFeed   — real-time L2 book + trade tape from Binance WebSocket
    LiveBook      — reconstructed L2 order book snapshot
    Trade         — a single trade print off the tape
    PaperGateway  — simulated fills against the live book + trade tape
    LiveEngine    — drives a Strategy on live data with risk + PnL
"""

from .feed    import LiveFeed, LiveBook, BookLevel, Trade, MarketEvent
from .binance import BinanceFeed
from .gateway import PaperGateway, GatewayFill
from .engine  import LiveEngine, LiveConfig

__all__ = [
    "LiveFeed", "LiveBook", "BookLevel", "Trade", "MarketEvent",
    "BinanceFeed",
    "PaperGateway", "GatewayFill",
    "LiveEngine", "LiveConfig",
]
