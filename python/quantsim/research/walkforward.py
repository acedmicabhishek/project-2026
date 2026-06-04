"""
Walk-forward testing and PurgedKFold cross-validation.

Walk-forward ensures a strategy is evaluated only on data it couldn't have seen
during training, preventing look-ahead bias in parameter tuning.

PurgedKFold additionally purges overlapping observations and adds an embargo gap
between train and test splits — the standard ML-finance anti-leakage technique.
"""

from __future__ import annotations

from typing import Callable, Iterator


class WalkForward:
    """
    Rolling walk-forward validator.

    For each window: fit strategy on train split, evaluate on test split.
    strategy_factory(train_ticks) must return a configured Strategy instance.

    Example:
        wf = WalkForward(n_splits=5, train_frac=0.7)
        results = wf.run(ticks, lambda _: EMACrossStrategy("AAPL", fast=10, slow=30))
        for r in results:
            print(r["window"], r["sharpe"], r["final_pnl"])
    """

    def __init__(self, n_splits: int = 5, train_frac: float = 0.7, gap: int = 0):
        """
        n_splits   : number of walk-forward windows
        train_frac : fraction of each window used for training
        gap        : ticks of embargo between train end and test start
        """
        self.n_splits   = n_splits
        self.train_frac = train_frac
        self.gap        = gap

    def run(self, ticks: list, strategy_factory: Callable,
            initial_cash: float = 500_000.0) -> list[dict]:
        """
        strategy_factory(train_ticks) → Strategy instance (already configured).
        Returns list of per-window result dicts with keys:
            window, train_start, train_end, test_start, test_end,
            n_train, n_test, final_pnl, sharpe, max_dd, n_fills, result
        """
        from quantsim.backtester import BacktestEngine
        from quantsim.analytics  import compute_metrics

        n        = len(ticks)
        win_size = n // self.n_splits
        results  = []

        for i in range(self.n_splits):
            start      = i * win_size
            end        = (start + win_size) if i < self.n_splits - 1 else n
            train_end  = start + int((end - start) * self.train_frac)
            test_start = min(train_end + self.gap, end)

            train_ticks = ticks[start:train_end]
            test_ticks  = ticks[test_start:end]

            if not train_ticks or not test_ticks:
                continue

            strategy = strategy_factory(train_ticks)
            engine   = BacktestEngine(initial_cash=initial_cash)
            result   = engine.run(test_ticks, strategy)
            m        = compute_metrics(result.equity)

            results.append({
                "window":      i,
                "train_start": start,
                "train_end":   train_end,
                "test_start":  test_start,
                "test_end":    end,
                "n_train":     len(train_ticks),
                "n_test":      len(test_ticks),
                "final_pnl":   result.final_pnl,
                "sharpe":      m.get("sharpe", 0.0),
                "max_dd":      m.get("max_drawdown", 0.0),
                "n_fills":     len(result.fills),
                "result":      result,
            })

        return results

    def summary(self, results: list[dict]) -> dict:
        """Aggregate walk-forward results into mean/std metrics."""
        if not results:
            return {}
        sharpes = [r["sharpe"]    for r in results]
        pnls    = [r["final_pnl"] for r in results]
        dds     = [r["max_dd"]    for r in results]
        import math
        def mean(xs): return sum(xs) / len(xs)
        def std(xs):
            m = mean(xs)
            return math.sqrt(sum((x - m)**2 for x in xs) / len(xs))
        return {
            "n_windows":   len(results),
            "mean_sharpe": round(mean(sharpes), 3),
            "std_sharpe":  round(std(sharpes),  3),
            "mean_pnl":    round(mean(pnls),     2),
            "std_pnl":     round(std(pnls),      2),
            "mean_max_dd": round(mean(dds),      4),
            "pct_positive":round(sum(1 for p in pnls if p > 0) / len(pnls) * 100, 1),
        }


class PurgedKFold:
    """
    Purged K-Fold cross-validation with embargo for time-series data.

    Prevents leakage by:
    1. Purging train samples whose forward-looking window overlaps the test set.
    2. Adding an embargo of `embargo_frac` × n_samples ticks after the test end
       before training resumes (prevents forward-looking in rolling features).

    Usage:
        pkf = PurgedKFold(n_splits=5, embargo_frac=0.01)
        for train_idx, test_idx in pkf.split(len(ticks)):
            train = [ticks[i] for i in train_idx]
            test  = [ticks[i] for i in test_idx]
    """

    def __init__(self, n_splits: int = 5, embargo_frac: float = 0.01):
        self.n_splits     = n_splits
        self.embargo_frac = embargo_frac

    def split(self, n_samples: int) -> Iterator[tuple[list[int], list[int]]]:
        """Yields (train_indices, test_indices) for each fold."""
        fold_size = n_samples // self.n_splits
        embargo   = max(1, int(n_samples * self.embargo_frac))

        for i in range(self.n_splits):
            test_start = i * fold_size
            test_end   = (test_start + fold_size) if i < self.n_splits - 1 else n_samples

            purge_start = max(0, test_start - embargo)
            resume      = min(n_samples, test_end + embargo)

            train_idx = list(range(0, purge_start)) + list(range(resume, n_samples))
            test_idx  = list(range(test_start, test_end))

            if train_idx and test_idx:
                yield train_idx, test_idx
