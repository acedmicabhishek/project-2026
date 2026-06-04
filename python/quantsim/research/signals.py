"""
Stateful streaming signal indicators.

Each class accepts one price per tick via .update(price) and returns a signal.
Signal convention:  +1 = long,  -1 = short/flat,  0 = no signal yet.

All indicators are O(1) per update (no list scans except BollingerBands).
"""

from __future__ import annotations
from collections import deque
import math


# ── Primitives ────────────────────────────────────────────────────────────────

class EMA:
    """Exponential moving average (Wilder initialisation: seed = first price)."""

    def __init__(self, period: int):
        self.period = period
        self.alpha  = 2.0 / (period + 1)
        self._value: float | None = None
        self._count = 0

    def update(self, price: float) -> float | None:
        self._count += 1
        if self._value is None:
            self._value = price
        else:
            self._value = self.alpha * price + (1.0 - self.alpha) * self._value
        return self._value if self._count >= self.period else None

    @property
    def value(self) -> float | None:
        return self._value if self._count >= self.period else None

    @property
    def ready(self) -> bool:
        return self._count >= self.period


class SMA:
    """Simple moving average — fixed-size circular buffer."""

    def __init__(self, period: int):
        self.period = period
        self._buf   = deque(maxlen=period)
        self._total = 0.0

    def update(self, price: float) -> float | None:
        if len(self._buf) == self.period:
            self._total -= self._buf[0]
        self._buf.append(price)
        self._total += price
        return self._total / self.period if len(self._buf) == self.period else None

    @property
    def value(self) -> float | None:
        return self._total / self.period if len(self._buf) == self.period else None

    @property
    def ready(self) -> bool:
        return len(self._buf) == self.period


class RSI:
    """Relative Strength Index (Wilder's exponential smoothing, period=14 default)."""

    def __init__(self, period: int = 14):
        self.period    = period
        self._prev:    float | None = None
        self._avg_gain = 0.0
        self._avg_loss = 0.0
        self._count    = 0
        self._warmup:  list[float] = []

    def update(self, price: float) -> float | None:
        if self._prev is None:
            self._prev = price
            return None

        delta = price - self._prev
        self._prev = price
        gain  = max(delta, 0.0)
        loss  = max(-delta, 0.0)
        self._count += 1

        if self._count < self.period:
            self._warmup.append((gain, loss))
            return None

        if self._count == self.period:
            self._warmup.append((gain, loss))
            self._avg_gain = sum(g for g, _ in self._warmup) / self.period
            self._avg_loss = sum(l for _, l in self._warmup) / self.period
            self._warmup.clear()
        else:
            self._avg_gain = (self._avg_gain * (self.period - 1) + gain) / self.period
            self._avg_loss = (self._avg_loss * (self.period - 1) + loss) / self.period

        if self._avg_loss == 0:
            return 100.0
        rs = self._avg_gain / self._avg_loss
        return 100.0 - (100.0 / (1.0 + rs))

    @property
    def ready(self) -> bool:
        return self._count >= self.period


class BollingerBands:
    """Bollinger Bands — returns (upper, middle, lower). Uses population std."""

    def __init__(self, period: int = 20, n_std: float = 2.0):
        self.period = period
        self.n_std  = n_std
        self._buf   = deque(maxlen=period)
        self._total = 0.0

    def update(self, price: float) -> tuple[float, float, float] | tuple[None, None, None]:
        if len(self._buf) == self.period:
            self._total -= self._buf[0]
        self._buf.append(price)
        self._total += price

        if len(self._buf) < self.period:
            return None, None, None

        mean = self._total / self.period
        std  = math.sqrt(sum((x - mean) ** 2 for x in self._buf) / self.period)
        return mean + self.n_std * std, mean, mean - self.n_std * std

    @property
    def ready(self) -> bool:
        return len(self._buf) == self.period


# ── Composite signals ─────────────────────────────────────────────────────────

class EMACross:
    """
    EMA crossover signal.
    Returns +1 when fast EMA > slow EMA, -1 otherwise, 0 while warming up.
    """

    def __init__(self, fast: int = 10, slow: int = 30):
        self._fast = EMA(fast)
        self._slow = EMA(slow)

    def update(self, price: float) -> int:
        f = self._fast.update(price)
        s = self._slow.update(price)
        if f is None or s is None:
            return 0
        return 1 if f > s else -1

    @property
    def ready(self) -> bool:
        return self._fast.ready and self._slow.ready


class MACross:
    """
    SMA crossover signal.
    Returns +1 when fast SMA > slow SMA, -1 otherwise, 0 while warming up.
    """

    def __init__(self, fast: int = 10, slow: int = 30):
        self._fast = SMA(fast)
        self._slow = SMA(slow)

    def update(self, price: float) -> int:
        f = self._fast.update(price)
        s = self._slow.update(price)
        if f is None or s is None:
            return 0
        return 1 if f > s else -1

    @property
    def ready(self) -> bool:
        return self._fast.ready and self._slow.ready


class RSISignal:
    """
    RSI threshold signal.
    Returns +1 when oversold (RSI < oversold), -1 when overbought (RSI > overbought), 0 neutral.
    """

    def __init__(self, period: int = 14, oversold: float = 30.0, overbought: float = 70.0):
        self._rsi       = RSI(period)
        self.oversold   = oversold
        self.overbought = overbought

    def update(self, price: float) -> int:
        rsi = self._rsi.update(price)
        if rsi is None:
            return 0
        if rsi < self.oversold:
            return 1
        if rsi > self.overbought:
            return -1
        return 0

    @property
    def ready(self) -> bool:
        return self._rsi.ready


class ZSpread:
    """
    Rolling z-score of a spread series.
    Returns -1 when z > +entry_z (spread high → short), +1 when z < -entry_z (spread low → long).
    """

    def __init__(self, window: int = 60, entry_z: float = 2.0):
        self.window  = window
        self.entry_z = entry_z
        self._buf    = deque(maxlen=window)
        self._total  = 0.0
        self._sq     = 0.0

    def update(self, spread: float) -> int:
        if len(self._buf) == self.window:
            old = self._buf[0]
            self._total -= old
            self._sq    -= old * old
        self._buf.append(spread)
        self._total += spread
        self._sq    += spread * spread

        if len(self._buf) < self.window:
            return 0

        mean = self._total / self.window
        var  = self._sq / self.window - mean * mean
        std  = math.sqrt(max(var, 0.0)) or 1e-9
        z    = (spread - mean) / std

        if z > self.entry_z:
            return -1
        if z < -self.entry_z:
            return 1
        return 0

    @property
    def ready(self) -> bool:
        return len(self._buf) >= self.window
