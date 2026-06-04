"""
Pure-Python synthetic market data generators.
Fallback when C++ SyntheticFeed not available; also provides OU spread feed.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing      import Iterator, List, Optional
import numpy as np
import math


@dataclass
class Tick:
    ts_ns:    int
    symbol:   str
    price:    float
    bid:      float
    ask:      float
    size:     int = 100
    is_trade: bool = False


class GBMFeed:
    """
    Geometric Brownian Motion price feed.
    dS = μ S dt + σ S dW  (Euler-Maruyama, log-normal step)
    """

    def __init__(self, symbol: str = "SYNTH", initial_price: float = 100.0,
                 mu: float = 0.0, sigma: float = 0.0005,
                 tick_size: float = 0.01, spread_ticks: float = 2.0,
                 dt: float = 1.0, seed: Optional[int] = None):
        self.symbol        = symbol
        self.price         = initial_price
        self.mu            = mu
        self.sigma         = sigma
        self.tick_size     = tick_size
        self.half_spread   = spread_ticks * tick_size / 2.0
        self.dt            = dt
        self._rng          = np.random.default_rng(seed)
        self._ts_ns        = 0

    def next(self) -> Tick:
        log_r  = (self.mu - 0.5 * self.sigma**2) * self.dt + \
                  self.sigma * math.sqrt(self.dt) * self._rng.standard_normal()
        self.price = max(self.tick_size,
                         round(self.price * math.exp(log_r) / self.tick_size) * self.tick_size)
        self._ts_ns += int(self.dt * 1_000_000)  # dt in ms → ns
        return Tick(
            ts_ns  = self._ts_ns,
            symbol = self.symbol,
            price  = self.price,
            bid    = round(self.price - self.half_spread, 4),
            ask    = round(self.price + self.half_spread, 4),
        )

    def generate(self, n: int) -> List[Tick]:
        return [self.next() for _ in range(n)]

    def __iter__(self) -> Iterator[Tick]:
        while True:
            yield self.next()


class OUSpreadFeed:
    """
    Two correlated assets where the spread follows an Ornstein-Uhlenbeck process.
    Used for pair trading / stat arb simulations.
    """

    def __init__(self, symbol_a: str = "A", symbol_b: str = "B",
                 initial_price: float = 100.0,
                 theta: float = 0.05, mu_spread: float = 0.0,
                 sigma_spread: float = 0.02, sigma_price: float = 0.001,
                 tick_size: float = 0.01, spread_ticks: float = 2,
                 dt: float = 1.0, seed: Optional[int] = None):
        self.symbols       = (symbol_a, symbol_b)
        self.price_a       = initial_price
        self.price_b       = initial_price
        self.spread        = mu_spread
        self.theta         = theta
        self.mu_spread     = mu_spread
        self.sigma_spread  = sigma_spread
        self.sigma_price   = sigma_price
        self.tick_size     = tick_size
        self.half_spread   = spread_ticks * tick_size / 2.0
        self.dt            = dt
        self._rng          = np.random.default_rng(seed)
        self._ts_ns        = 0

    def next(self) -> tuple[Tick, Tick]:
        dW1, dW2, dW_spread = self._rng.standard_normal(3)

        # Common price trend
        common = self.sigma_price * math.sqrt(self.dt) * dW1
        self.price_a = max(self.tick_size, self.price_a * (1 + common + self.sigma_price * dW2 * 0.3))
        self.price_b = max(self.tick_size, self.price_a + self.spread)

        # OU spread update
        self.spread += (self.theta * (self.mu_spread - self.spread) * self.dt +
                        self.sigma_spread * math.sqrt(self.dt) * dW_spread)

        self._ts_ns += int(self.dt * 1_000_000)

        def make_tick(sym, price):
            return Tick(
                ts_ns  = self._ts_ns,
                symbol = sym,
                price  = round(price / self.tick_size) * self.tick_size,
                bid    = round((price - self.half_spread) / self.tick_size) * self.tick_size,
                ask    = round((price + self.half_spread) / self.tick_size) * self.tick_size,
            )
        return make_tick(self.symbols[0], self.price_a), \
               make_tick(self.symbols[1], self.price_b)

    def generate(self, n: int) -> tuple[List[Tick], List[Tick]]:
        ticks_a, ticks_b = [], []
        for _ in range(n):
            a, b = self.next()
            ticks_a.append(a)
            ticks_b.append(b)
        return ticks_a, ticks_b

    @property
    def current_spread(self) -> float:
        return self.spread
