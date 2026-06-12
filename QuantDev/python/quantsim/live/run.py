"""
Live crypto session runner.

Connects a Binance live feed to a strategy through the live engine and streams
the session to stdout (the GUI parses it). Also runnable standalone in a terminal.

    python -m quantsim.live.run --symbol BTCUSDT --strategy mm --duration 60
    python -m quantsim.live.run --symbol ETHUSDT --strategy momentum --no-stream

Strategy ids: mm | momentum | meanrev | twap
"""

from __future__ import annotations

import argparse
import asyncio
import signal
import sys

from .binance   import BinanceFeed
from .engine    import LiveEngine, LiveConfig
from .risk      import RiskLimits
from .protocol  import StreamWriter
from .strategies import REGISTRY


def build_strategy(name: str, args):
    name = name.lower()
    if name not in REGISTRY:
        raise SystemExit(f"unknown strategy '{name}'. choices: {', '.join(REGISTRY)}")
    cls = REGISTRY[name]

    if name == "mm":
        return cls(quote_size=args.size, spread_bps=args.spread_bps,
                   max_inventory=args.max_position)
    if name == "momentum":
        return cls(trade_size=args.size, max_position=args.max_position)
    if name == "meanrev":
        return cls(trade_size=args.size, max_position=args.max_position)
    if name == "twap":
        return cls(side=args.side, target_qty=args.qty,
                   duration_s=args.duration or 120.0)
    return cls()


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="QuantSim live crypto session")
    p.add_argument("--symbol",   default="BTCUSDT")
    p.add_argument("--strategy", default="mm",
                   choices=list(REGISTRY.keys()))
    p.add_argument("--duration", type=float, default=0.0,
                   help="seconds to run (0 = until Ctrl-C / GUI stop)")
    p.add_argument("--cash",     type=float, default=100_000.0)
    p.add_argument("--size",     type=float, default=0.002,
                   help="per-order base size")
    p.add_argument("--spread-bps", dest="spread_bps", type=float, default=4.0,
                   help="MM target quoted spread (bps)")
    p.add_argument("--qty",      type=float, default=0.05,
                   help="TWAP parent quantity")
    p.add_argument("--side",     default="B", help="TWAP side B/S")
    p.add_argument("--max-position", dest="max_position", type=float, default=0.02)
    p.add_argument("--maker-fee", type=float, default=0.0,  help="maker fee bps")
    p.add_argument("--taker-fee", type=float, default=5.0,  help="taker fee bps")
    p.add_argument("--max-notional", type=float, default=100_000.0)
    p.add_argument("--max-loss",  type=float, default=5_000.0)
    p.add_argument("--futures",   action="store_true")
    p.add_argument("--depth",     type=int, default=20, choices=[5, 10, 20])
    p.add_argument("--no-stream", action="store_true",
                   help="human-readable summary instead of protocol stream")
    return p.parse_args(argv)


async def _main(args) -> int:
    feed = BinanceFeed(args.symbol, depth=args.depth, futures=args.futures)
    strat = build_strategy(args.strategy, args)

    limits = RiskLimits(
        max_position=args.max_position,
        max_order_qty=max(args.size, args.qty),
        max_notional=args.max_notional,
        max_daily_loss=args.max_loss,
    )
    cfg = LiveConfig(
        symbol=args.symbol, initial_cash=args.cash,
        duration_s=args.duration,
        maker_fee_bps=args.maker_fee, taker_fee_bps=args.taker_fee,
        stream=not args.no_stream, risk=limits,
    )
    writer = StreamWriter(enabled=not args.no_stream)
    engine = LiveEngine(feed, strat, cfg, writer)

    # graceful stop on SIGINT/SIGTERM
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, engine.stop)
        except (NotImplementedError, ValueError):
            pass

    result = await engine.run()

    if args.no_stream:
        m = result.get("metrics", {})
        print("\n── session summary ─────────────────────────")
        print(f"  symbol     : {args.symbol}")
        print(f"  strategy   : {args.strategy}")
        print(f"  PnL        : ${result['pnl']:,.2f}")
        print(f"  realized   : ${result['realized']:,.2f}")
        print(f"  fees       : ${result['fees']:,.2f}")
        print(f"  position   : {result['position']:+.6f}")
        print(f"  fills      : {result['fills']}")
        print(f"  orders     : {result['orders']}  (blocked {result['blocked']})")
        if m:
            print(f"  sharpe     : {m.get('sharpe', 0):.3f}")
            print(f"  max dd     : {m.get('max_drawdown', 0)*100:.2f}%")
        print("────────────────────────────────────────────")
    return 0


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        return asyncio.run(_main(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
