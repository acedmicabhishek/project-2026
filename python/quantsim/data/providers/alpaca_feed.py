"""
Real market data via Alpaca Markets.

Requires a free Alpaca paper-trading account:
    https://app.alpaca.markets/signup

Set credentials via environment variables:
    export ALPACA_API_KEY="PKxxxxxxxxxxxxxxxx"
    export ALPACA_SECRET_KEY="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

Or pass directly to AlpacaFeed(api_key=..., secret_key=...).

Paper vs live:
    AlpacaFeed(paper=True)   → paper.alpaca.markets  (default, safest)
    AlpacaFeed(paper=False)  → api.alpaca.markets     (live data)

Dependencies:
    pip install alpaca-py pyarrow
"""
from __future__ import annotations

import logging
import os
import queue
import threading
from datetime import datetime, timedelta, timezone
from typing import Callable, Iterator, List, Optional

from .base import DataProvider, TickCache, INTERVAL_NS, bar_to_ticks
from ..synthetic import Tick

log = logging.getLogger(__name__)

# alpaca-py TimeFrame strings
_TF_MAP: dict[str, str] = {
    "1m":  "1Min",
    "5m":  "5Min",
    "15m": "15Min",
    "30m": "30Min",
    "1h":  "1Hour",
    "1d":  "1Day",
}

_PERIOD_DAYS: dict[str, int] = {
    "1d":  1,   "5d":  5,   "1mo": 30,  "3mo": 90,
    "6mo": 180, "1y":  365, "2y":  730, "5y":  1825,
}


class AlpacaFeed(DataProvider):
    """
    Historical bars and live quote stream from Alpaca Markets.

    Historical (fetch):
        Downloads OHLCV bars via alpaca-py REST → synthesizes Tick sequences.
        Identical schema to YFinanceFeed — drop-in replacement.

    Live (stream):
        Opens a WebSocket quote subscription and yields Tick objects in real-time.
        Run in a background thread; call stop_stream() to close.

    Usage:
        feed  = AlpacaFeed()                             # reads env vars
        ticks = feed.fetch("NVDA", period="1y", interval="1d")
        ticks = feed.fetch("AAPL", period="5d", interval="1m")

        # Live stream
        for tick in feed.stream("AAPL"):
            strategy.on_tick(tick)
    """

    def __init__(self,
                 api_key:         Optional[str] = None,
                 secret_key:      Optional[str] = None,
                 paper:           bool          = True,
                 cache:           bool          = True,
                 min_half_spread: float         = 0.005):
        self._api_key    = api_key    or os.environ.get("ALPACA_API_KEY",    "")
        self._secret_key = secret_key or os.environ.get("ALPACA_SECRET_KEY", "")
        self._paper      = paper
        self._cache      = TickCache() if cache else None
        self._min_half_spread = min_half_spread
        self._stop_event = threading.Event()

    # ── Historical ────────────────────────────────────────────────────────────

    def fetch(self, symbol: str, period: str = "1y",
              interval: str = "1d") -> List[Tick]:
        """
        Fetch historical bars for `symbol`.

        Args:
            symbol:   Ticker, e.g. "AAPL".
            period:   "1d","5d","1mo","3mo","6mo","1y","2y","5y".
            interval: "1m","5m","15m","30m","1h","1d".

        Returns:
            List[Tick] sorted by timestamp.
        """
        cache_key = f"alpaca_{period}"
        if self._cache:
            cached = self._cache.load(symbol, cache_key, interval)
            if cached:
                return cached

        self._require_alpaca_py()

        from alpaca.data.historical import StockHistoricalDataClient
        from alpaca.data.requests   import StockBarsRequest
        from alpaca.data.timeframe  import TimeFrame, TimeFrameUnit

        client = StockHistoricalDataClient(
            self._api_key or None,
            self._secret_key or None,
        )

        tf = self._parse_timeframe(interval, TimeFrame, TimeFrameUnit)
        now   = datetime.now(timezone.utc)
        start = now - timedelta(days=_PERIOD_DAYS.get(period, 365))

        log.info("fetching %s %s/%s from Alpaca…", symbol, period, interval)
        req  = StockBarsRequest(symbol_or_symbols=symbol, timeframe=tf,
                                start=start, end=now)
        bars = client.get_stock_bars(req)
        df   = bars.df

        if df is None or df.empty:
            raise ValueError(
                f"No Alpaca data for {symbol} ({period}/{interval}).\n"
                "Ensure your API key is valid and the symbol is tradeable on US markets."
            )

        # Alpaca returns a MultiIndex DataFrame: (symbol, timestamp)
        if hasattr(df.index, "levels"):
            try:
                df = df.xs(symbol, level="symbol")
            except KeyError:
                pass  # single-symbol — index is already timestamp

        bar_ns      = INTERVAL_NS.get(interval, INTERVAL_NS["1d"])
        avg_price   = float(df["close"].mean())
        half_spread = max(self._min_half_spread, avg_price * 0.0001)

        ticks: List[Tick] = []
        for ts, row in df.iterrows():
            try:
                ts_ns = int(ts.timestamp() * 1_000_000_000)
            except Exception:
                continue
            ticks.extend(bar_to_ticks(
                symbol, ts_ns, bar_ns,
                float(row["open"]),  float(row["high"]),
                float(row["low"]),   float(row["close"]),
                int(row.get("volume", 0)),
                half_spread,
            ))

        if not ticks:
            raise ValueError(f"Zero ticks for {symbol} via Alpaca.")

        log.info("fetched %d ticks for %s via Alpaca", len(ticks), symbol)

        if self._cache:
            self._cache.save(ticks, symbol, cache_key, interval)
        return ticks

    # ── Live streaming ────────────────────────────────────────────────────────

    def stream(self, symbol: str,
               on_tick: Optional[Callable[[Tick], None]] = None) -> Iterator[Tick]:
        """
        Stream live quotes via Alpaca WebSocket.

        Yields Tick objects as quotes arrive. Runs the WebSocket client
        in a background thread so this generator stays blocking-safe.

        Args:
            symbol:  Ticker to subscribe, e.g. "AAPL".
            on_tick: Optional callback fired for every tick (in addition to yield).

        Usage:
            for tick in feed.stream("AAPL"):
                strategy.on_tick(tick)
                if shutdown:
                    feed.stop_stream()
                    break
        """
        self._require_alpaca_py()

        from alpaca.data.live import StockDataStream

        q: queue.Queue[Optional[Tick]] = queue.Queue(maxsize=10_000)
        self._stop_event.clear()

        async def _on_quote(data):
            try:
                ts_ns = int(data.timestamp.timestamp() * 1_000_000_000)
                bid   = float(getattr(data, "bid_price", 0.0))
                ask   = float(getattr(data, "ask_price", 0.0))
                if bid <= 0 or ask <= 0:
                    return
                price = round((bid + ask) / 2.0, 4)
                tick  = Tick(
                    ts_ns    = ts_ns,
                    symbol   = symbol,
                    price    = price,
                    bid      = round(bid, 4),
                    ask      = round(ask, 4),
                    size     = int(getattr(data, "bid_size", 0) + getattr(data, "ask_size", 0)),
                    is_trade = False,
                )
                q.put_nowait(tick)
                if on_tick:
                    on_tick(tick)
            except Exception as e:
                log.warning("stream parse error: %s", e)

        client_feed = "iex" if self._paper else "sip"
        ws = StockDataStream(self._api_key, self._secret_key, feed=client_feed)
        ws.subscribe_quotes(_on_quote, symbol)

        def _run():
            try:
                ws.run()
            except Exception as e:
                log.error("stream error: %s", e)
            finally:
                q.put(None)  # sentinel → stop iteration

        t = threading.Thread(target=_run, daemon=True, name=f"alpaca-stream-{symbol}")
        t.start()
        log.info("Alpaca WebSocket stream started for %s (feed=%s)", symbol, client_feed)

        while not self._stop_event.is_set():
            try:
                tick = q.get(timeout=1.0)
            except queue.Empty:
                continue
            if tick is None:
                break
            yield tick

        log.info("stream stopped for %s", symbol)

    def stop_stream(self) -> None:
        """Signal the live stream to stop."""
        self._stop_event.set()

    # ── Helpers ───────────────────────────────────────────────────────────────

    @staticmethod
    def _require_alpaca_py():
        try:
            import alpaca  # noqa: F401
        except ImportError:
            raise ImportError(
                "alpaca-py is not installed.\n"
                "Install with: pip install alpaca-py"
            )

    @staticmethod
    def _parse_timeframe(interval: str, TimeFrame, TimeFrameUnit):
        mapping = {
            "1m":  (1,  TimeFrameUnit.Minute),
            "5m":  (5,  TimeFrameUnit.Minute),
            "15m": (15, TimeFrameUnit.Minute),
            "30m": (30, TimeFrameUnit.Minute),
            "1h":  (1,  TimeFrameUnit.Hour),
            "1d":  (1,  TimeFrameUnit.Day),
        }
        if interval not in mapping:
            raise ValueError(
                f"Unsupported interval '{interval}' for Alpaca.\n"
                f"Supported: {list(mapping)}"
            )
        amount, unit = mapping[interval]
        return TimeFrame(amount, unit)
