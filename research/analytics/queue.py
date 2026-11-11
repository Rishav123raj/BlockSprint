"""Queue analytics (P5.7): where passive orders joined the queue, how long they waited, and how often they filled.

    from research.analytics import queue
    queue.placements(run)            # one row per passive placement: queue ahead at entry, time in queue, outcome
    queue.fill_curve(run)            # fill probability against the queue ahead at entry (quantile bins)
    queue.fill_curve(run, by="ticks")   # ... against the distance from the best price
    queue.summary(run)               # one row per strategy

* **Passive placements:** limit and post-only placements (research.analytics.tca.orders) that were accepted and
  never took liquidity.
* **Queue ahead at entry** is measured on the data, not inside the simulator: the displayed quantity already
  at the placement's price when it reached the exchange (the book after every data batch up to its arrival;
  blocksprint_sim.data_level_qty). It is the same for every queue model; how fast that quantity drains is
  what the models differ in. `ahead_ratio` is it in multiples of the placement's own quantity.
* **ticks_from_best:** 0 joined the best price, > 0 behind it, < 0 improved it (a new level: nothing ahead).
* **Time in queue:** from arrival to the first fill, or to the end (cancel, replace, the run's end).
* **Traded at / through the price** while it rested (the data's trades): trades at its price by the other side's
  aggressors (a sell aggressor for a resting buy) and at worse prices for them. `fifo_reachable`: together they
  exceed the queue ahead, so trades alone would have reached it under first-in-first-out. Placements that
  were not reachable still fill: the queue ahead also shrinks when the displayed level is cancelled down past
  it (how much depends on the queue model) and when the market moves through the price.
"""

from __future__ import annotations

from typing import Any, Sequence

import numpy as np

from research.analytics.market import MarketData
from research.analytics.tca import _EPS, orders as _orders


def _tick(run: Any) -> float:
    return float(run.meta["data_header"]["tick_size"])


def placements(run: Any, market: MarketData | None = None):
    """One row per passive placement: tca.orders' columns plus ahead_qty, ahead_ratio, best (the data's best
    price on its side at arrival), ticks_from_best, time_in_queue_ms, outcome (filled / partial / unfilled),
    traded_at_price, traded_through (base units, while it rested) and fifo_reachable."""
    import polars as pl

    market = market or MarketData.for_run(run)
    o = _orders(run, market).filter(
        pl.col("order_type").is_in(["limit", "limit_maker"]) & (pl.col("status") != "rejected")
        & ((pl.col("filled_qty") - pl.col("maker_qty")) <= _EPS))
    arrive = o["arrive_ns"].to_numpy()
    buy = o["side"].to_numpy() == "buy"
    px = o["price"].to_numpy()
    qty = o["quantity"].to_numpy()
    ahead = market.level_qty(arrive, np.where(buy, 0, 1), px)
    bbo = market.bbo_at(arrive)
    best = np.where(buy, bbo["bid"], bbo["ask"])
    ticks = np.round(np.where(buy, best - px, px - best) / _tick(run))
    first = o["first_fill_ns"].fill_null(o["end_ns"]).to_numpy()
    end = o["end_ns"].to_numpy()
    filled = o["filled_qty"].to_numpy()

    t = market.trades()
    tts, tpx, tqty, tbuy = t["ts_ns"], t["px"], t["qty"], t["buy"]
    at_price = np.zeros(len(o))
    through = np.zeros(len(o))
    lo = np.searchsorted(tts, arrive, side="left")
    hi = np.searchsorted(tts, end, side="left")
    for i in range(len(o)):
        a, b = lo[i], hi[i]
        if a >= b:
            continue
        # A resting buy is hit by sell aggressors: at its price, or below it (through it)
        hit = ~tbuy[a:b] if buy[i] else tbuy[a:b]
        p = tpx[a:b]
        at_price[i] = tqty[a:b][hit & (p == px[i])].sum()
        through[i] = tqty[a:b][hit & ((p < px[i]) if buy[i] else (p > px[i]))].sum()

    outcome = np.where(filled >= qty - _EPS, "filled", np.where(filled > _EPS, "partial", "unfilled"))
    return o.with_columns(
        pl.Series("ahead_qty", ahead),
        pl.Series("ahead_ratio", ahead / qty),
        pl.Series("best", best),
        pl.Series("ticks_from_best", ticks),
        pl.Series("time_in_queue_ms", (first - arrive) / 1e6),
        pl.Series("outcome", outcome),
        pl.Series("traded_at_price", at_price),
        pl.Series("traded_through", through),
        pl.Series("fifo_reachable", at_price + through > ahead),
    ).with_columns(pl.col("ahead_qty", "ahead_ratio", "best", "ticks_from_best").fill_nan(None)).with_columns(
        pl.col("ticks_from_best").cast(pl.Int64))


def fill_curve(run: Any, by: str = "ahead", bins: int | Sequence[float] = 10, market: MarketData | None = None,
               placements_df=None):
    """Fill probability against the queue at entry, per strategy.

    by="ahead": bins of ahead_qty (base units): `bins` quantile bins (int), or bin edges (a list; a bin is
    [edge, next edge), the last includes its end). Placements that improved the best price (nothing ahead) form
    their own bin. by="ticks": one row per ticks_from_best.

    Columns: owner, bin (a label), lo, hi, placements, fill_rate (any fill), full_fill_rate, mean_ahead,
    mean_ahead_ratio, median_time_in_queue_ms, median_time_to_fill_ms (the filled ones), fifo_reachable (share)."""
    import polars as pl

    p = placements_df if placements_df is not None else placements(run, market)
    p = p.filter(pl.col("ahead_qty").is_not_null())
    if by == "ticks":
        keyed = p.with_columns(pl.col("ticks_from_best").cast(pl.Float64).alias("lo"),
                               pl.col("ticks_from_best").cast(pl.Float64).alias("hi"),
                               pl.col("ticks_from_best").cast(pl.String).alias("bin"))
    elif by == "ahead":
        frames = []
        for owner in p["owner"].unique(maintain_order=True).to_list():
            mine = p.filter(pl.col("owner") == owner)
            empty = mine.filter(pl.col("ahead_qty") <= _EPS)
            rest = mine.filter(pl.col("ahead_qty") > _EPS)
            a = rest["ahead_qty"].to_numpy()
            if isinstance(bins, int):
                edges = np.unique(np.quantile(a, np.linspace(0, 1, bins + 1))) if len(a) else np.array([])
                if len(edges) == 1:  # all the same queue: one bin holding it
                    edges = np.array([edges[0], edges[0]])
            else:
                edges = np.asarray(sorted(bins), dtype=np.float64)
            if len(edges) >= 2:
                k = np.clip(np.searchsorted(edges, a, side="right") - 1, 0, len(edges) - 2)
                inside = (a >= edges[0]) & (a <= edges[-1])
                rest = rest.with_columns(pl.Series("lo", edges[k]), pl.Series("hi", edges[k + 1])).filter(
                    pl.Series(inside))
                frames.append(rest.with_columns(
                    pl.format("[{}, {})", pl.col("lo").round(8), pl.col("hi").round(8)).alias("bin")))
            if empty.height:
                frames.append(empty.with_columns(pl.lit(0.0).alias("lo"), pl.lit(0.0).alias("hi"),
                                                 pl.lit("0 (new level)").alias("bin")))
        keyed = pl.concat(frames, how="diagonal_relaxed") if frames else p.head(0).with_columns(
            pl.lit(0.0).alias("lo"), pl.lit(0.0).alias("hi"), pl.lit("").alias("bin"))  # no rows: just the columns
    else:
        raise ValueError(f"by must be 'ahead' or 'ticks', not {by!r}")
    filled = pl.col("filled_qty") > _EPS
    return keyed.group_by(["owner", "bin", "lo", "hi"]).agg(
        pl.len().cast(pl.Int64).alias("placements"),
        filled.mean().alias("fill_rate"),
        (pl.col("outcome") == "filled").mean().alias("full_fill_rate"),
        pl.col("ahead_qty").mean().alias("mean_ahead"),
        pl.col("ahead_ratio").mean().alias("mean_ahead_ratio"),
        pl.col("time_in_queue_ms").median().alias("median_time_in_queue_ms"),
        pl.col("time_in_queue_ms").filter(filled).median().alias("median_time_to_fill_ms"),
        pl.col("fifo_reachable").mean().alias("fifo_reachable"),
    ).sort(["owner", "lo", "hi"])


def summary(run: Any, market: MarketData | None = None, placements_df=None):
    """One row per strategy: passive placements, the shares that joined the best price, improved it or sat behind
    it, median queue ahead (base units and in own sizes), median time in queue, fill_rate, full_fill_rate,
    fifo_reachable (share) and fill_rate_reachable / fill_rate_unreachable (fill rates of those two groups)."""
    import polars as pl

    p = placements_df if placements_df is not None else placements(run, market)
    filled = pl.col("filled_qty") > _EPS
    reach = pl.col("fifo_reachable")
    return p.group_by("owner", maintain_order=True).agg(
        pl.len().cast(pl.Int64).alias("placements"),
        (pl.col("ticks_from_best") == 0).mean().alias("at_best"),
        (pl.col("ticks_from_best") < 0).mean().alias("improved"),
        (pl.col("ticks_from_best") > 0).mean().alias("behind"),
        pl.col("ahead_qty").median().alias("median_ahead"),
        pl.col("ahead_ratio").median().alias("median_ahead_ratio"),
        pl.col("time_in_queue_ms").median().alias("median_time_in_queue_ms"),
        filled.mean().alias("fill_rate"),
        (pl.col("outcome") == "filled").mean().alias("full_fill_rate"),
        reach.mean().alias("fifo_reachable"),
        filled.filter(reach).mean().alias("fill_rate_reachable"),
        filled.filter(~reach).mean().alias("fill_rate_unreachable"),
    )
