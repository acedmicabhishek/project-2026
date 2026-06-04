"""
Pre-trade risk manager (Python).

Every order passes through `check()` before it reaches the gateway. Any failed
check blocks the order; a breached loss limit arms the kill switch, which halts
all new quoting until disarmed. This is the layer that turns a strategy that can
blow up into one that gets stopped first.

Mirrors the intent of the C++ `PreTradeRisk` / `KillSwitch` in
include/hft_simulator/risk.hpp; kept in Python here so the live session owns its
own risk state without a binding round-trip on every order.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing      import Optional, Tuple


@dataclass
class RiskLimits:
    max_position:     float = 1.0        # max |net position| in base units (e.g. BTC)
    max_order_qty:    float = 0.5        # max single-order size
    max_notional:     float = 100_000.0  # max single-order notional ($)
    max_orders_per_s: float = 25.0       # token-bucket order-rate cap
    fat_finger_bps:   float = 200.0      # reject limit price > this many bps off mid
    max_daily_loss:   float = 5_000.0    # arm kill switch when realized+unreal loss exceeds


class TokenBucket:
    """Classic token bucket for order-rate limiting."""

    def __init__(self, rate_per_s: float, burst: Optional[float] = None):
        self.rate   = max(rate_per_s, 0.001)
        self.burst  = burst if burst is not None else max(rate_per_s, 1.0)
        self.tokens = self.burst
        self.ts     = time.monotonic()

    def take(self, n: float = 1.0) -> bool:
        now = time.monotonic()
        self.tokens = min(self.burst, self.tokens + (now - self.ts) * self.rate)
        self.ts = now
        if self.tokens >= n:
            self.tokens -= n
            return True
        return False


class RiskManager:
    def __init__(self, limits: RiskLimits):
        self.limits  = limits
        self._bucket = TokenBucket(limits.max_orders_per_s)
        self.kill_armed = False
        self.kill_reason = ""
        self.blocks = 0          # count of blocked orders (observability)

    def arm_kill(self, reason: str) -> None:
        if not self.kill_armed:
            self.kill_armed = True
            self.kill_reason = reason

    def disarm(self) -> None:
        self.kill_armed = False
        self.kill_reason = ""

    def on_pnl(self, total_pnl: float) -> None:
        """Call each mark. Arms the kill switch if the loss limit is breached."""
        if total_pnl <= -abs(self.limits.max_daily_loss):
            self.arm_kill(f"daily loss limit hit ({total_pnl:.2f})")

    def check(self, is_buy: bool, qty: float, price: float, mid: float,
              net_position: float, ord_type: str) -> Tuple[bool, str]:
        """
        Returns (ok, reason). reason is "" when ok.
        """
        L = self.limits
        if self.kill_armed:
            return False, f"KILL:{self.kill_reason}"

        if qty <= 0:
            return False, "qty<=0"

        if qty > L.max_order_qty:
            self.blocks += 1
            return False, f"order qty {qty:g} > max {L.max_order_qty:g}"

        ref_px = price if (price > 0 and ord_type == "LIMIT") else mid
        notional = qty * (ref_px if ref_px > 0 else mid)
        if notional > L.max_notional:
            self.blocks += 1
            return False, f"notional {notional:,.0f} > max {L.max_notional:,.0f}"

        signed = qty if is_buy else -qty
        new_pos = net_position + signed
        if abs(new_pos) > L.max_position + 1e-9:
            self.blocks += 1
            return False, f"position {new_pos:+g} > max |{L.max_position:g}|"

        if ord_type == "LIMIT" and mid > 0 and price > 0:
            off_bps = abs(price - mid) / mid * 1e4
            if off_bps > L.fat_finger_bps:
                self.blocks += 1
                return False, f"fat-finger {off_bps:.0f}bps off mid"

        if not self._bucket.take(1.0):
            self.blocks += 1
            return False, "order rate limit"

        return True, ""

    @property
    def state(self) -> str:
        return "ARMED" if self.kill_armed else "OK"
