"""
Stream protocol — the wire format between a live session (Python subprocess) and
the GUI (C++) that renders it.

One line per record, newline-terminated, written to stdout. The GUI parses by
prefix. Backward compatible with the original EQ:/BA:/FILL: lines; adds richer
records for a live crypto session.

  EQ:{equity}                              mark-to-market equity point
  MID:{mid}                                current mid price
  BA:{bid},{ask}                           best bid / best ask
  DEPTH:{p:q|p:q|...};{p:q|p:q|...}        full L2 ladder  (bids ; asks)
  FILL:{side}:{qty}:{price}:{symbol}:{m}   a fill (m=1 maker, 0 taker)
  POS:{net}:{avg_px}:{realized}:{unreal}   position + pnl breakdown
  STAT:{k}={v};{k}={v};...                 metrics (sharpe, fills, orders, ...)
  RISK:{state}:{detail}                    OK | ARMED | BLOCKED, with reason
  LAT:{feed_us}                            feed latency, microseconds
  LOG:{text}                               free-text log line

Anything without a known prefix is treated as a plain log line by the GUI.
"""

from __future__ import annotations

import sys
from typing import List, Tuple


class StreamWriter:
    """Writes protocol lines to stdout (or any text stream), flushing each line."""

    def __init__(self, stream=None, enabled: bool = True):
        self._out = stream if stream is not None else sys.stdout
        self._enabled = enabled

    def _w(self, line: str) -> None:
        if not self._enabled:
            return
        self._out.write(line + "\n")
        self._out.flush()

    # ── records ───────────────────────────────────────────────────────────────

    def equity(self, eq: float) -> None:
        self._w(f"EQ:{eq:.4f}")

    def mid(self, mid: float) -> None:
        self._w(f"MID:{mid:.4f}")

    def best_quote(self, bid: float, ask: float) -> None:
        self._w(f"BA:{bid:.4f},{ask:.4f}")

    def depth(self, bids: List[Tuple[float, float]],
              asks: List[Tuple[float, float]], levels: int = 12) -> None:
        b = "|".join(f"{p:.4f}:{q:.6f}" for p, q in bids[:levels])
        a = "|".join(f"{p:.4f}:{q:.6f}" for p, q in asks[:levels])
        self._w(f"DEPTH:{b};{a}")

    def fill(self, side: str, qty: float, price: float,
             symbol: str, is_maker: bool) -> None:
        self._w(f"FILL:{side}:{qty:.6f}:{price:.4f}:{symbol}:{1 if is_maker else 0}")

    def position(self, net: float, avg_px: float,
                 realized: float, unreal: float) -> None:
        self._w(f"POS:{net:.6f}:{avg_px:.4f}:{realized:.4f}:{unreal:.4f}")

    def stat(self, **kw) -> None:
        body = ";".join(f"{k}={v}" for k, v in kw.items())
        self._w(f"STAT:{body}")

    def risk(self, state: str, detail: str = "") -> None:
        self._w(f"RISK:{state}:{detail}")

    def latency(self, feed_us: float) -> None:
        self._w(f"LAT:{feed_us:.1f}")

    def log(self, text: str) -> None:
        self._w(f"LOG:{text}")
