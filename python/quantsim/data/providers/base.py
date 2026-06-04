"""
Abstract base for real market data providers.
Shared: bar→tick synthesis, Parquet-backed disk cache.
"""
from __future__ import annotations

import logging
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Iterator, List, Optional

from ..synthetic import Tick

log = logging.getLogger(__name__)

CACHE_DIR = Path.home() / ".quantsim" / "data"

# Bar duration in nanoseconds
INTERVAL_NS: dict[str, int] = {
    "1m":  60_000_000_000,
    "2m":  120_000_000_000,
    "5m":  300_000_000_000,
    "15m": 900_000_000_000,
    "30m": 1_800_000_000_000,
    "1h":  3_600_000_000_000,
    "90m": 5_400_000_000_000,
    "1d":  86_400_000_000_000,
    "1wk": 604_800_000_000_000,
}


def bar_to_ticks(
    symbol: str,
    ts_ns: int,
    bar_ns: int,
    o: float,
    h: float,
    l: float,
    c: float,
    vol: int,
    half_spread: float = 0.01,
) -> List[Tick]:
    """
    Synthesize 4 intrabar ticks from a single OHLCV bar.

    Price path (OHLC anatomy):
      Up-bar  (c >= o): open → low  → high → close
      Down-bar (c < o): open → high → low  → close

    Timestamps are evenly spaced across bar_ns.
    This approximates real intrabar price dynamics (dip before rally / spike
    before decline) and is the standard used in Zipline/Backtrader.
    """
    is_up  = c >= o
    path   = [o, (l if is_up else h), (h if is_up else l), c]
    qty_ea = max(1, vol // 4)
    ticks: List[Tick] = []
    for i, price in enumerate(path):
        t_ns = ts_ns + int(bar_ns * (i + 1) / 4)
        ticks.append(Tick(
            ts_ns    = t_ns,
            symbol   = symbol,
            price    = round(price, 4),
            bid      = round(price - half_spread, 4),
            ask      = round(price + half_spread, 4),
            size     = qty_ea,
            is_trade = (i == 3),   # mark close tick as a trade event
        ))
    return ticks


class TickCache:
    """
    Parquet-backed disk cache for Tick lists.
    Falls back to no-op if pyarrow is not installed.
    """

    def __init__(self, cache_dir: Path = CACHE_DIR):
        self._dir = cache_dir
        self._dir.mkdir(parents=True, exist_ok=True)
        try:
            import pyarrow         as _pa   # noqa: F401
            import pyarrow.parquet as _pq   # noqa: F401
            self._pa  = _pa
            self._pq  = _pq
            self._ok  = True
        except ImportError:
            self._ok = False
            log.debug("pyarrow not installed — cache disabled. pip install pyarrow")

    def _path(self, symbol: str, period: str, interval: str) -> Path:
        key = f"{symbol.upper()}_{interval}_{period}".replace("/", "-")
        return self._dir / f"{key}.parquet"

    def load(self, symbol: str, period: str, interval: str) -> Optional[List[Tick]]:
        if not self._ok:
            return None
        p = self._path(symbol, period, interval)
        if not p.exists():
            return None
        try:
            d = self._pq.read_table(p).to_pydict()
            ticks = [
                Tick(
                    ts_ns    = int(d["ts_ns"][i]),
                    symbol   = d["symbol"][i],
                    price    = float(d["price"][i]),
                    bid      = float(d["bid"][i]),
                    ask      = float(d["ask"][i]),
                    size     = int(d["size"][i]),
                    is_trade = bool(d["is_trade"][i]),
                )
                for i in range(len(d["ts_ns"]))
            ]
            log.info("cache hit: %d ticks ← %s", len(ticks), p.name)
            return ticks
        except Exception as e:
            log.warning("cache load failed (%s): %s", p.name, e)
            return None

    def save(self, ticks: List[Tick], symbol: str, period: str, interval: str) -> None:
        if not self._ok or not ticks:
            return
        p = self._path(symbol, period, interval)
        try:
            table = self._pa.table({
                "ts_ns":    [t.ts_ns    for t in ticks],
                "symbol":   [t.symbol   for t in ticks],
                "price":    [t.price    for t in ticks],
                "bid":      [t.bid      for t in ticks],
                "ask":      [t.ask      for t in ticks],
                "size":     [t.size     for t in ticks],
                "is_trade": [t.is_trade for t in ticks],
            })
            self._pq.write_table(table, p)
            log.info("cached %d ticks → %s", len(ticks), p.name)
        except Exception as e:
            log.warning("cache save failed: %s", e)


class DataProvider(ABC):
    """Abstract base for all real market data providers."""

    @abstractmethod
    def fetch(self, symbol: str, period: str = "1y",
              interval: str = "1d") -> List[Tick]:
        """Fetch historical bars and return as a list of synthesized Ticks."""

    def stream(self, symbol: str) -> Iterator[Tick]:
        """Stream live ticks. Not all providers support this."""
        raise NotImplementedError(f"{type(self).__name__} does not support streaming")
