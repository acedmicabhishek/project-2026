"""
Real market data via yfinance (free, no API key required).

Limitations of Yahoo Finance free tier:
  1m  data: last 7 days only
  5m  data: last 60 days
  1h  data: last 730 days (2 years)
  1d+ data: full history

Usage:
    feed  = YFinanceFeed()
    ticks = feed.fetch("AAPL", period="1y",  interval="1d")
    ticks = feed.fetch("NVDA", period="5d",  interval="1m")
    ticks = feed.fetch("SPY",  period="6mo", interval="1h")
"""
from __future__ import annotations

import logging
from typing import List

from .base import DataProvider, TickCache, INTERVAL_NS, bar_to_ticks
from ..synthetic import Tick

log = logging.getLogger(__name__)

# Symbols known to work well on Yahoo Finance
COMMON_SYMBOLS: list[str] = [
    "AAPL", "MSFT", "NVDA", "GOOG", "AMZN", "META", "TSLA", "AMD",
    "JPM", "BAC", "GS", "MS", "BRK-B", "WFC",
    "SPY", "QQQ", "IWM", "GLD", "TLT", "VXX",
    "BTC-USD", "ETH-USD",
]


class YFinanceFeed(DataProvider):
    """
    Downloads OHLCV bars from Yahoo Finance and synthesizes intrabar tick
    sequences compatible with BacktestEngine.

    Each OHLCV bar → 4 Ticks spread across the bar's time window.
    Bid/ask synthesized as price ± dynamic half-spread (1 bps of mid).
    Results cached as Parquet in ~/.quantsim/data/ for fast reloads.
    """

    def __init__(self, cache: bool = True,
                 min_half_spread: float = 0.005):
        """
        Args:
            cache:            Save/load results from Parquet cache.
            min_half_spread:  Floor on bid/ask half-spread in dollars.
                              Real spread = max(this, 1 bps of price).
        """
        self._cache           = TickCache() if cache else None
        self._min_half_spread = min_half_spread

    # ── Public API ────────────────────────────────────────────────────────────

    def fetch(self, symbol: str, period: str = "1y",
              interval: str = "1d") -> List[Tick]:
        """
        Fetch historical data for `symbol`.

        Args:
            symbol:   Ticker symbol, e.g. "AAPL", "BTC-USD".
            period:   yfinance period string: "1d","5d","1mo","3mo","6mo",
                      "1y","2y","5y","10y","ytd","max".
            interval: Bar size: "1m","5m","15m","30m","1h","1d","1wk","1mo".

        Returns:
            List[Tick] in chronological order.

        Raises:
            ImportError: if yfinance not installed.
            ValueError:  if no data returned.
        """
        # Cache hit?
        if self._cache:
            cached = self._cache.load(symbol, period, interval)
            if cached:
                return cached

        try:
            import yfinance as yf
        except ImportError:
            raise ImportError(
                "yfinance is not installed.\n"
                "Install with: pip install yfinance"
            )

        log.info("fetching %s %s/%s from Yahoo Finance…", symbol, period, interval)
        ticker = yf.Ticker(symbol)
        df = ticker.history(period=period, interval=interval,
                            auto_adjust=True, repair=True)

        if df.empty:
            raise ValueError(
                f"No data returned for {symbol} ({period}/{interval}).\n"
                f"Note: 1m data only available for last 7 days."
            )

        bar_ns     = INTERVAL_NS.get(interval, INTERVAL_NS["1d"])
        avg_price  = float(df["Close"].mean())
        half_spread = max(self._min_half_spread, avg_price * 0.0001)  # 1 bps floor

        ticks: List[Tick] = []
        for ts, row in df.iterrows():
            try:
                ts_ns = int(ts.timestamp() * 1_000_000_000)
            except Exception:
                continue

            o   = float(row["Open"])
            h   = float(row["High"])
            l   = float(row["Low"])
            c   = float(row["Close"])
            vol = int(row.get("Volume", 0))

            if o <= 0 or c <= 0 or h < l:
                continue  # bad bar — skip

            ticks.extend(bar_to_ticks(symbol, ts_ns, bar_ns,
                                       o, h, l, c, vol, half_spread))

        if not ticks:
            raise ValueError(f"Zero ticks synthesized for {symbol}. Check data quality.")

        log.info("fetched %d ticks for %s", len(ticks), symbol)

        if self._cache:
            self._cache.save(ticks, symbol, period, interval)

        return ticks

    def available_symbols(self) -> list[str]:
        """Return list of commonly used symbols."""
        return list(COMMON_SYMBOLS)

    def info(self, symbol: str) -> dict:
        """Return fundamental info for a symbol (company name, sector, market cap, etc.)."""
        try:
            import yfinance as yf
            return yf.Ticker(symbol).info
        except ImportError:
            raise ImportError("pip install yfinance")
