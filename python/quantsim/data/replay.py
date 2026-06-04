"""
CSV tick replay (pure Python wrapper around C++ CsvTickReplay or standalone).
"""

from __future__ import annotations

import csv
from typing import Callable, Iterator, List, Optional
from dataclasses import dataclass


@dataclass
class Tick:
    ts_ns:    int
    symbol:   str
    price:    float
    bid:      float
    ask:      float
    size:     int     = 100
    is_trade: bool    = False


class TickReplay:
    """
    Load CSV tick data and replay it.

    CSV header: ts_ns,symbol,price,bid,ask,size,is_trade

    Usage:
        replay = TickReplay("ticks.csv")
        for tick in replay:
            strategy.on_book_update(...)
    """

    def __init__(self, path: Optional[str] = None):
        self._ticks: List[Tick] = []
        if path:
            self.load(path)

    def load(self, path: str):
        self._ticks.clear()
        with open(path, newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                self._ticks.append(Tick(
                    ts_ns    = int(row['ts_ns']),
                    symbol   = row['symbol'],
                    price    = float(row['price']),
                    bid      = float(row['bid']),
                    ask      = float(row['ask']),
                    size     = int(row.get('size', 100)),
                    is_trade = row.get('is_trade', '0') in ('1', 'true', 'True'),
                ))

    def save(self, path: str, ticks: Optional[List[Tick]] = None):
        data = ticks or self._ticks
        with open(path, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['ts_ns','symbol','price','bid','ask','size','is_trade'])
            for t in data:
                writer.writerow([t.ts_ns, t.symbol, t.price, t.bid, t.ask,
                                  t.size, int(t.is_trade)])

    def __iter__(self) -> Iterator[Tick]:
        return iter(self._ticks)

    def __len__(self) -> int:
        return len(self._ticks)

    def __getitem__(self, idx):
        return self._ticks[idx]

    @property
    def ticks(self) -> List[Tick]:
        return self._ticks
