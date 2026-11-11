"""Markouts (P5.6): what each fill was worth some time later, marked at the mid.

    from research.analytics.markouts import fill_markouts, markout_curve
    fill_markouts(run)                        # each fill with its markout at every horizon
    markout_curve(run)                        # averages per horizon, by liquidity and side
    markout_curve(run, np.geomspace(1e6, 6e10, 30).astype(int), by=())   # a dense curve, all fills together

The markout of a fill at horizon h is s x (m(t + h) - price), s = +1 for a buy and -1 for a sell, m the data's
mid (research.analytics.market): the fill's profit if the position were closed at the mid h later. **Positive
is good for the strategy** (unlike tca.py's costs). In basis points it is divided by m(t), the mid at the fill.

* At h = 0 it is the fill's spread capture (research.analytics.pnl); later horizons add the mid's move,
  s x (m(t + h) - m(t)): a maker's curve falling with h is adverse selection, a taker's rising is alpha.
* Horizons default to 0, 100 ms, 1 s, 5 s, 30 s and 60 s. A horizon past the end of the data has no mid:
  that markout is null and left out of the averages (`fills` counts the ones used).
* Fees are not included (pnl.py has them).
"""

from __future__ import annotations

from datetime import timedelta
from typing import Any, Iterable, Sequence

import numpy as np

from research.analytics.market import MarketData
from research.analytics.pnl import fills as _fills
from research.analytics.pnl import parse_every

HORIZONS = ("0ms", "100ms", "1s", "5s", "30s", "60s")


def _label(ns: int) -> str:
    for unit, size in (("s", 1_000_000_000), ("ms", 1_000_000), ("us", 1_000)):  # 60 s is "60s"
        if ns >= size and ns % size == 0:
            return f"{ns // size}{unit}"
    return f"{ns}ns" if ns else "0ms"


def _horizons(horizons: Iterable[str | int | timedelta]) -> list[int]:
    out = []
    for h in horizons:
        ns = 0 if h in (0, "0", "0ms", "0s") else parse_every(h)
        if ns not in out:
            out.append(ns)
    return sorted(out)


def fill_markouts(run: Any, horizons: Sequence[str | int | timedelta] = HORIZONS, market: MarketData | None = None):
    """Each fill (research.analytics.pnl.fills) with two columns per horizon: mo_<h>_bps (basis points of the
    mid at the fill) and mo_<h> (quote units, x quantity), e.g. mo_5s_bps, mo_60s, mo_100ms_bps. Null past the
    end of the data."""
    import polars as pl

    market = market or MarketData.for_run(run)
    f = _fills(run, market)
    t = f["exchange_ns"].to_numpy()
    q = f["qty"].to_numpy()
    s = np.sign(q)
    px, mid = f["price"].to_numpy(), f["mid"].to_numpy()
    cols = []
    for h in _horizons(horizons):
        later = market.mid(t + h)
        if market.end_ns is not None:
            later = np.where(t + h <= market.end_ns, later, np.nan)
        per_unit = s * (later - px)
        name = f"mo_{_label(h)}"
        cols += [pl.Series(f"{name}_bps", per_unit / mid * 1e4).fill_nan(None),
                 pl.Series(name, per_unit * np.abs(q)).fill_nan(None)]
    return f.with_columns(cols)


def markout_curve(run: Any, horizons: Sequence[str | int | timedelta] = HORIZONS,
                  by: Sequence[str] = ("liquidity", "side"), market: MarketData | None = None):
    """The average markout per horizon, per strategy and group (by: fill columns, e.g. ("liquidity", "side"),
    ("liquidity",) or () for all fills together).

    Columns: owner, the `by` columns, horizon (a label such as "5s"), horizon_ns, fills (with a mid at that
    horizon), qty, mean_bps (quantity-weighted), median_bps, se_bps (standard error of the unweighted mean),
    pnl (quote units: the sum of the fills' markouts)."""
    import polars as pl

    hs = _horizons(horizons)
    f = fill_markouts(run, hs, market)
    keys = ["owner", *by]
    frames = []
    for h in hs:
        name = f"mo_{_label(h)}"
        g = f.filter(pl.col(f"{name}_bps").is_not_null()).with_columns(pl.col("qty").abs().alias("_q"))
        frames.append(g.group_by(keys, maintain_order=True).agg(
            pl.lit(_label(h)).alias("horizon"),
            pl.lit(h, pl.Int64).alias("horizon_ns"),
            pl.len().cast(pl.Int64).alias("fills"),
            pl.col("_q").sum().alias("qty"),
            ((pl.col(f"{name}_bps") * pl.col("_q")).sum() / pl.col("_q").sum()).alias("mean_bps"),
            pl.col(f"{name}_bps").median().alias("median_bps"),
            (pl.col(f"{name}_bps").std() / pl.len().sqrt()).alias("se_bps"),
            pl.col(name).sum().alias("pnl"),
        ))
    if not frames:
        return pl.DataFrame()
    return pl.concat(frames).sort([*keys, "horizon_ns"], maintain_order=True)
