"""The market a run saw (P5.4, P5.5): the data's best bid and ask at any moment, its trades, and the run's
tables as floats.

    from research.analytics.market import MarketData
    md = MarketData.for_run(run)              # replays the run's .bse file (BTCUSDT, a whole day: ~5 s)
    md.mid(ts_ns)                             # the data's mid at each time, as the simulator marks it

The timeline comes from blocksprint_sim.data_bbo: the book rebuilt from the file with the simulator's rules,
one row per change of the best bid or ask. Its mid is the simulator's mark (positions.csv), the same integer
in 1e-8 units. Two lookups at a time t, because events at the same nanosecond happen in order:

* side="right": the book after every batch at or before t. A fill at t sees this: the data batch that
  caused it has been applied.
* side="left": the book after every batch before t. The simulator's samples (positions.csv, bbo_samples.csv)
  and its final mark see this.

While the book isn't valid (before the first snapshot, in a gap) the mid is carried forward from the last
valid one, as the simulator's mark is (it would use the engine book's mid first, which has no strategy
orders here; it can differ only inside a gap).
"""

from __future__ import annotations

import functools
from pathlib import Path
from typing import Any, Literal

import numpy as np

import blocksprint_sim as bs

SCALE = 100_000_000  # fixed point: 1e-8 units
Side = Literal["left", "right"]


def run_window(run: Any) -> tuple[int, int, int]:
    """(start_ns, trading_ns, end_ns) of a run: the replayed window, the end of the warm-up and the end."""
    w = run.meta["window"]
    return int(w["start_ns"]), int(w["trading_ns"]), int(w["end_ns"])


def table(run: Any, name: str):
    """One of a run's tables as polars, with fixed-point columns as Float64 and `*_ns` columns as Int64 ns."""
    import polars as pl

    if hasattr(run, "to_polars"):        # Results
        df = run.to_polars(name, fixed="float")
    else:                                # StoredRun or alike: Decimal columns
        df = run.frame(name)
    casts = []
    for col, dtype in df.schema.items():
        if isinstance(dtype, pl.Datetime):
            casts.append(pl.col(col).dt.epoch("ns").alias(col))
        elif isinstance(dtype, pl.Decimal):
            casts.append(pl.col(col).cast(pl.Float64))
    return df.with_columns(casts) if casts else df


AFTER_NS = 60_000_000_000  # how far past a run's end MarketData.for_run reads by default (markout horizons)


@functools.lru_cache(maxsize=4)
def _timeline(path: str, from_ns: int, to_ns: int) -> np.ndarray:
    return bs.data_bbo(path, from_ns, to_ns)


@functools.lru_cache(maxsize=4)
def _trades(path: str, from_ns: int, to_ns: int) -> np.ndarray:
    return bs.data_trades(path, from_ns, to_ns)


class MarketData:
    """The data's BBO over time and its trades. Prices and quantities as floats in quote / base units, NaN for
    no level."""

    def __init__(self, bbo: np.ndarray, trades: np.ndarray | None = None, end_ns: int | None = None):
        """bbo: a blocksprint_sim.data_bbo array (ts_ns, bid, bid_qty, ask, ask_qty in 1e-8 units); trades: a
        blocksprint_sim.data_trades array (ts_ns, px, qty, side), or None to read them when first needed;
        end_ns: where the data stops (later times have no mid for markouts), None: no limit."""
        self.bbo = bbo
        self.end_ns = end_ns
        self._trades_raw = trades
        self._source: tuple[str, int, int] | None = None
        self.ts = bbo["ts_ns"].astype(np.int64)
        bid, ask = bbo["bid"], bbo["ask"]
        valid = (bid > 0) & (ask > 0)
        # The simulator's mark: (bid + ask) / 2 in integer 1e-8 units, rounded down
        raw = np.where(valid, (bid + ask) // 2, 0).astype(np.float64) / SCALE
        last = np.maximum.accumulate(np.where(valid, np.arange(len(bbo)), -1))
        self._mid = np.where(last >= 0, raw[np.maximum(last, 0)], np.nan)

    @classmethod
    def for_run(cls, run: Any, path: str | Path | None = None, after_ns: int = AFTER_NS) -> MarketData:
        """The timeline over a run's window, plus after_ns past its end (for markouts; the data may end
        sooner). path: the .bse file, if the run's (run_meta.json data.file, relative to the working directory
        then) has moved. Timelines are cached, so the analytics of one run share one replay."""
        start, _, end = run_window(run)
        source = (str(path or run.meta["data"]["file"]), start, end + after_ns)
        last = run.meta.get("data_header", {}).get("last_ts_local")
        md = cls(_timeline(*source), end_ns=min(source[2], int(last)) if last is not None else source[2])
        md._source = source
        return md

    def trades(self) -> dict[str, np.ndarray]:
        """The data's trades: ts_ns, px, qty (floats) and buy (True: the aggressor bought)."""
        if self._trades_raw is None:
            if self._source is None:
                raise ValueError("MarketData: no trades given and no file to read them from")
            self._trades_raw = _trades(*self._source)
        t = self._trades_raw
        return {"ts_ns": t["ts_ns"].astype(np.int64), "px": t["px"].astype(np.float64) / SCALE,
                "qty": t["qty"].astype(np.float64) / SCALE, "buy": t["side"] == 0}

    def vwap(self, from_ns: int, to_ns: int) -> tuple[float, float]:
        """(VWAP, volume) of the data's trades with from_ns <= ts < to_ns; VWAP NaN without trades."""
        t = self.trades()
        lo, hi = np.searchsorted(t["ts_ns"], [from_ns, to_ns], side="left")
        qty = t["qty"][lo:hi]
        vol = float(qty.sum())
        return (float((t["px"][lo:hi] * qty).sum() / vol) if vol else float("nan")), vol

    def twap(self, from_ns: int, to_ns: int) -> float:
        """The mid's time-weighted average over [from_ns, to_ns) (NaN if it has no valid mid there)."""
        if to_ns <= from_ns:
            return float(self.mid([from_ns])[0])
        lo = int(np.searchsorted(self.ts, from_ns, side="right"))  # the changes inside (from_ns, to_ns)
        hi = int(np.searchsorted(self.ts, to_ns, side="left"))
        times = np.concatenate([[from_ns], self.ts[lo:hi], [to_ns]]).astype(np.float64)
        values = np.concatenate([self.mid([from_ns]), self._mid[lo:hi]])
        w = np.diff(times)
        ok = ~np.isnan(values) & (w > 0)
        return float((values[ok] * w[ok]).sum() / w[ok].sum()) if ok.any() else float("nan")

    def _index(self, ts_ns: Any, side: Side) -> np.ndarray:
        return np.searchsorted(self.ts, np.asarray(ts_ns, dtype=np.int64), side=side) - 1

    def mid(self, ts_ns: Any, side: Side = "right") -> np.ndarray:
        """The mid at each time (NaN before the book is first valid)."""
        i = self._index(ts_ns, side)
        return np.where(i >= 0, self._mid[np.maximum(i, 0)], np.nan)

    def bbo_at(self, ts_ns: Any, side: Side = "right") -> dict[str, np.ndarray]:
        """bid, bid_qty, ask, ask_qty at each time (NaN for no level or no valid book; not carried forward)."""
        i = self._index(ts_ns, side)
        rows = self.bbo[np.maximum(i, 0)]
        out = {}
        for col in ("bid", "bid_qty", "ask", "ask_qty"):
            v = rows[col].astype(np.float64) / SCALE
            out[col] = np.where((i >= 0) & (rows[col] > 0), v, np.nan)
        return out
