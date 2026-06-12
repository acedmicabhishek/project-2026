"""
Binance live market-data adapter.

Subscribes to a *combined* WebSocket stream:
  - <symbol>@depth20@100ms : top-20 L2 book snapshot, pushed every 100ms
  - <symbol>@trade         : every trade print (for the tape / maker-fill model)

Partial-depth streams are self-contained snapshots, so there is no sequence-number
bookkeeping to get wrong (unlike the diff-depth stream). Plenty for strategy
signals and a live ladder.

No API key required for market data. Spot endpoint by default; pass
`futures=True` for USD-M perpetuals (adds funding/perp semantics elsewhere).
"""

from __future__ import annotations

import asyncio
import json
import logging
from typing import AsyncIterator, List

import websockets

from .feed import LiveFeed, LiveBook, BookLevel, Trade, MarketEvent, now_ns

log = logging.getLogger(__name__)

SPOT_WS    = "wss://stream.binance.com:9443/stream"
FUTURES_WS = "wss://fstream.binance.com/stream"


class BinanceFeed(LiveFeed):
    def __init__(self,
                 symbol: str = "BTCUSDT",
                 depth: int = 20,
                 throttle_ms: int = 100,
                 futures: bool = False,
                 with_trades: bool = True,
                 reconnect: bool = True):
        self._symbol  = symbol.upper()
        self._depth   = depth if depth in (5, 10, 20) else 20
        self._throt   = 100 if throttle_ms >= 100 else throttle_ms
        self._base    = FUTURES_WS if futures else SPOT_WS
        self._trades  = with_trades
        self._reconnect = reconnect
        self._stop    = False

    @property
    def symbol(self) -> str:
        return self._symbol

    def stop(self) -> None:
        self._stop = True
        if hasattr(self, "_ws") and self._ws:
            try:
                loop = asyncio.get_event_loop()
                if loop.is_running():
                    loop.create_task(self._ws.close())
            except Exception:
                pass

    def _url(self) -> str:
        s = self._symbol.lower()
        streams = [f"{s}@depth{self._depth}@{self._throt}ms"]
        if self._trades:
            streams.append(f"{s}@trade")
        return f"{self._base}?streams={'/'.join(streams)}"

    async def events(self) -> AsyncIterator[MarketEvent]:
        backoff = 1.0
        while not self._stop:
            try:
                async with websockets.connect(
                    self._url(),
                    open_timeout=10,
                    ping_interval=20,
                    ping_timeout=20,
                    max_queue=1024,
                ) as ws:
                    self._ws = ws
                    log.info("Binance feed connected: %s", self._url())
                    backoff = 1.0
                    async for raw in ws:
                        if self._stop:
                            break
                        ev = self._parse(raw)
                        if ev is not None:
                            yield ev
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 — network resilience
                if self._stop or not self._reconnect:
                    log.warning("Binance feed stopped: %s", e)
                    return
                log.warning("Binance feed dropped (%s) — reconnecting in %.1fs", e, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 30.0)

    # ── parsing ───────────────────────────────────────────────────────────────

    def _parse(self, raw: str) -> MarketEvent | None:
        recv = now_ns()
        try:
            msg = json.loads(raw)
        except (json.JSONDecodeError, TypeError):
            return None

        stream = msg.get("stream", "")
        data   = msg.get("data", msg)   # tolerate non-combined payloads

        if "@depth" in stream or ("bids" in data and "asks" in data):
            return self._parse_book(data, recv)
        if "@trade" in stream or data.get("e") == "trade":
            return self._parse_trade(data, recv)
        return None

    def _parse_book(self, d: dict, recv: int) -> LiveBook | None:
        try:
            bids = [BookLevel(float(p), float(q)) for p, q in d["bids"]]
            asks = [BookLevel(float(p), float(q)) for p, q in d["asks"]]
        except (KeyError, ValueError, TypeError):
            return None
        # depth20 snapshot carries no event time; use receive time
        return LiveBook(symbol=self._symbol, ts_ns=recv,
                        bids=bids, asks=asks, recv_ns=recv)

    def _parse_trade(self, d: dict, recv: int) -> Trade | None:
        try:
            return Trade(
                symbol=self._symbol,
                ts_ns=int(d["T"]) * 1_000_000,   # exchange trade time (ms → ns)
                price=float(d["p"]),
                qty=float(d["q"]),
                is_buyer_maker=bool(d["m"]),
                recv_ns=recv,
            )
        except (KeyError, ValueError, TypeError):
            return None


# ── Convenience: list active USDT pairs (REST, no key) ────────────────────────

async def top_usdt_symbols(limit: int = 20) -> List[str]:
    """Return the top USDT spot symbols by 24h quote volume. Best-effort."""
    import urllib.request
    url = "https://api.binance.com/api/v3/ticker/24hr"
    try:
        loop = asyncio.get_event_loop()
        raw = await loop.run_in_executor(
            None, lambda: urllib.request.urlopen(url, timeout=10).read())
        rows = json.loads(raw)
        usdt = [r for r in rows if r["symbol"].endswith("USDT")]
        usdt.sort(key=lambda r: float(r.get("quoteVolume", 0)), reverse=True)
        return [r["symbol"] for r in usdt[:limit]]
    except Exception as e:  # noqa: BLE001
        log.warning("symbol fetch failed: %s", e)
        return ["BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT"]
