"""
QuantSim data management CLI.

Usage:
    quantsim-data pull NVDA --interval 1d --period 1y
    quantsim-data pull AAPL MSFT --interval 1h --source alpaca
    quantsim-data list
    quantsim-data clear
    quantsim-data info AAPL
"""
from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")


def _get_feed(source: str):
    if source == "yfinance":
        from quantsim.data.providers.yfinance_feed import YFinanceFeed
        return YFinanceFeed()
    if source == "alpaca":
        from quantsim.data.providers.alpaca_feed import AlpacaFeed
        return AlpacaFeed()
    print(f"Unknown source: {source!r}. Use 'yfinance' or 'alpaca'.")
    sys.exit(1)


def cmd_pull(args) -> None:
    feed = _get_feed(args.source)
    for sym in args.symbols:
        print(f"Pulling {sym} ({args.period}/{args.interval}) from {args.source}…")
        try:
            ticks = feed.fetch(sym, period=args.period, interval=args.interval)
            print(f"  {sym}: {len(ticks):,} ticks  ✓")
        except Exception as e:
            print(f"  {sym}: ERROR — {e}")


def cmd_list(_args) -> None:
    from quantsim.data.providers.base import CACHE_DIR
    if not CACHE_DIR.exists():
        print("No cache directory. Run 'quantsim-data pull' first.")
        return
    files = sorted(CACHE_DIR.glob("*.parquet"))
    if not files:
        print("Cache is empty.")
        return
    print(f"\n{'Dataset':<50} {'Size':>10}")
    print("─" * 62)
    for f in files:
        size_kb = f.stat().st_size / 1024
        print(f"  {f.stem:<48} {size_kb:>8.1f} KB")
    print(f"\n{len(files)} dataset(s) in {CACHE_DIR}")


def cmd_clear(_args) -> None:
    from quantsim.data.providers.base import CACHE_DIR
    if not CACHE_DIR.exists():
        print("Nothing to clear.")
        return
    files = list(CACHE_DIR.glob("*.parquet"))
    for f in files:
        f.unlink()
    print(f"Cleared {len(files)} cached file(s).")


def cmd_info(args) -> None:
    try:
        from quantsim.data.providers.yfinance_feed import YFinanceFeed
        info = YFinanceFeed().info(args.symbol)
        for key in ("longName", "sector", "industry", "marketCap",
                    "trailingPE", "dividendYield", "beta", "currency"):
            val = info.get(key, "—")
            if isinstance(val, float):
                val = f"{val:,.4f}"
            print(f"  {key:<20} {val}")
    except Exception as e:
        print(f"Error: {e}")


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="quantsim-data",
        description="QuantSim real market data management",
    )
    sub = parser.add_subparsers(dest="cmd", metavar="<command>")

    pull_p = sub.add_parser("pull", help="Download and cache market data")
    pull_p.add_argument("symbols", nargs="+", metavar="SYMBOL",
                        help="Ticker symbols, e.g. AAPL NVDA SPY")
    pull_p.add_argument("--source",   default="yfinance",
                        choices=["yfinance", "alpaca"],
                        help="Data provider (default: yfinance)")
    pull_p.add_argument("--period",   default="1y",
                        help="History length: 1d,5d,1mo,3mo,6mo,1y,2y,5y (default: 1y)")
    pull_p.add_argument("--interval", default="1d",
                        help="Bar size: 1m,5m,15m,30m,1h,1d (default: 1d)")

    sub.add_parser("list",  help="List cached datasets")
    sub.add_parser("clear", help="Delete all cached data")

    info_p = sub.add_parser("info", help="Print fundamental info for a symbol")
    info_p.add_argument("symbol", metavar="SYMBOL")

    args = parser.parse_args()

    dispatch = {
        "pull":  cmd_pull,
        "list":  cmd_list,
        "clear": cmd_clear,
        "info":  cmd_info,
    }
    fn = dispatch.get(args.cmd)
    if fn:
        fn(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
