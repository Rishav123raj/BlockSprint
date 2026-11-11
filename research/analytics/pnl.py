"""PnL and accounting (P5.4): mark-to-mid PnL split into spread capture, inventory PnL and fees.

    from research.analytics.pnl import decompose, pnl_buckets, fills
    decompose(run)                  # one row per strategy: pnl = spread_capture + inventory - fees
    pnl_buckets(run, "1m")          # the same per time bucket: the run's returns
    fills(run)                      # each fill with the mid at that moment and its edge

All values are in quote units (USDT). With the data's mid m(t) as the mark (as positions.csv's pnl):

* **spread capture** of a fill: q x (m(t_fill) - price), q signed (+ buy). What the fill earned against the
  mid at that moment: positive for buying below the mid or selling above it. Split by maker and taker.
* **inventory PnL** over [a, b): the position held, times the mid's moves: pos(a) x (m(b) - m(a)), plus
  q x (m(b) - m(t_fill)) for each fill in [a, b).
* **fees** paid (negative: rebates).

The three add up to the change in mark-to-mid value cash + position x m - fees over every bucket, exactly
(up to the 1e-8 rounding of each fill's notional), so the buckets' pnl sums to the run's. `residual` is the
engine's final pnl (positions.csv) minus the sum: about 0 unless the mark came from somewhere else (inside a
data gap the simulator marks at the engine book's mid).

A fill's mid is the book after every data batch up to and including its time (the batch that caused it);
a bucket edge's mid is the book before batches at that time, as the simulator's samples see it. So the
cumulative pnl at the edges equals positions.csv's pnl at the sample times.
"""

from __future__ import annotations

import re
from datetime import timedelta
from typing import Any

import numpy as np

from research.analytics.market import MarketData, run_window, table

_UNITS = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000, "m": 60_000_000_000,
          "h": 3_600_000_000_000, "d": 86_400_000_000_000}


def parse_every(every: str | int | timedelta) -> int:
    """A bucket length as ns: an int (ns), a timedelta, or a string such as "500ms", "1s", "5m", "1h"."""
    if isinstance(every, timedelta):
        ns = int(round(every.total_seconds() * 1e9))
    elif isinstance(every, (int, np.integer)):
        ns = int(every)
    else:
        m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*(ns|us|ms|s|m|h|d)\s*", str(every))
        if not m:
            raise ValueError(f"bucket length {every!r}: expected e.g. '500ms', '1s', '5m', '1h', or ns")
        ns = int(round(float(m.group(1)) * _UNITS[m.group(2)]))
    if ns <= 0:
        raise ValueError(f"bucket length {every!r}: must be positive")
    return ns


def fills(run: Any, market: MarketData | None = None):
    """Every fill (exec_reports type == "fill") with the mid at its time and its spread capture.

    Columns: exchange_ns, owner, order_id, side, liquidity, qty (signed, + buy), price, fee, mid,
    spread_capture (qty x (mid - price)), edge_bps (the price's distance from the mid in the fill's favour, in
    basis points of the mid)."""
    import polars as pl

    market = market or MarketData.for_run(run)
    rep = table(run, "exec_reports").filter(pl.col("type") == "fill")
    t = rep["exchange_ns"].to_numpy()
    sign = np.where(rep["side"].to_numpy() == "buy", 1.0, -1.0)
    qty = sign * rep["last_qty"].to_numpy()
    px = rep["last_px"].to_numpy()
    mid = market.mid(t, "right")
    spread = qty * (mid - px)
    return pl.DataFrame({
        "exchange_ns": t,
        "owner": rep["owner"],
        "order_id": rep["order_id"],
        "side": rep["side"],
        "liquidity": rep["liquidity"],
        "qty": qty,
        "price": px,
        "fee": rep["fee"].fill_null(0.0).to_numpy(),
        "mid": mid,
        "spread_capture": spread,
        "edge_bps": sign * (mid - px) / mid * 1e4,
    })


def _owners(run: Any) -> list[str]:
    pos = table(run, "positions")
    return list(dict.fromkeys(pos["owner"].to_list()))


def pnl_buckets(run: Any, every: str | int | timedelta = "1m", market: MarketData | None = None,
                owner: str | None = None, start: str = "trading"):
    """The PnL split per time bucket, per strategy: the run's returns.

    Buckets are [edge, edge + every) from the start of trading (start="trading", the default; "window"
    includes the warm-up) to the end of the run; the last one may be shorter.

    Columns: owner, start_ns, end_ns, fills, base_volume, spread_capture, inventory, fees, pnl (the bucket's
    return: spread_capture + inventory - fees), cum_pnl, position and mid (both at the bucket's end)."""
    import polars as pl

    every_ns = parse_every(every)
    if start not in ("trading", "window"):
        raise ValueError(f"start must be 'trading' or 'window', not {start!r}")
    market = market or MarketData.for_run(run)
    w_start, w_trading, w_end = run_window(run)
    first = w_trading if start == "trading" else w_start
    edges = np.arange(first, w_end, every_ns, dtype=np.int64)
    edges = np.append(edges, np.int64(w_end)) if len(edges) == 0 or edges[-1] < w_end else edges
    if len(edges) < 2:
        edges = np.array([first, max(w_end, first + 1)], dtype=np.int64)
    edge_mid = np.nan_to_num(market.mid(edges, "left"))  # before the book is valid nothing is held

    f = fills(run, market)
    frames = []
    for name in ([owner] if owner is not None else _owners(run)):
        mine = f.filter(pl.col("owner") == name)
        t = mine["exchange_ns"].to_numpy()
        if len(t) and t[0] < first:
            raise ValueError(f"{name}: a fill at {t[0]} before the first bucket ({first}); use start='window'")
        q = mine["qty"].to_numpy()
        fill_mid = mine["mid"].to_numpy()
        k = np.searchsorted(edges, t, side="right") - 1
        nb = len(edges) - 1
        k = np.minimum(k, nb - 1)  # a fill at the very end belongs to the last bucket
        q_b = np.bincount(k, q, nb)
        pos_edge = np.concatenate([[0.0], np.cumsum(q_b)])  # position at each edge
        spread_b = np.bincount(k, np.nan_to_num(mine["spread_capture"].to_numpy()), nb)
        fees_b = np.bincount(k, mine["fee"].to_numpy(), nb)
        carried = pos_edge[:-1] * np.diff(edge_mid)
        traded = np.bincount(k, q * (edge_mid[k + 1] - np.nan_to_num(fill_mid, nan=0.0)), nb) if len(t) else 0.0
        inventory_b = carried + traded
        pnl_b = spread_b + inventory_b - fees_b
        frames.append(pl.DataFrame({
            "owner": [name] * nb,
            "start_ns": edges[:-1],
            "end_ns": edges[1:],
            "fills": np.bincount(k, minlength=nb).astype(np.int64),
            "base_volume": np.bincount(k, np.abs(q), nb),
            "spread_capture": spread_b,
            "inventory": inventory_b,
            "fees": fees_b,
            "pnl": pnl_b,
            "cum_pnl": np.cumsum(pnl_b),
            "position": pos_edge[1:],
            "mid": edge_mid[1:],
        }))
    return pl.concat(frames) if frames else pl.DataFrame()


def decompose(run: Any, market: MarketData | None = None):
    """The run's PnL split, one row per strategy.

    Columns: owner, pnl (the engine's, positions.csv at the end), spread_capture (= spread_maker +
    spread_taker), inventory, fees, residual (pnl - (spread_capture + inventory - fees)), fills, maker_fills,
    taker_fills, base_volume, quote_volume, edge_bps (spread capture per unit of notional traded, in basis
    points), final position and mark."""
    import polars as pl

    market = market or MarketData.for_run(run)
    f = fills(run, market)
    buckets = pnl_buckets(run, run_window(run)[2] - run_window(run)[0] + 1, market, start="window")
    pos = table(run, "positions")
    rows = []
    for name in _owners(run):
        final = pos.filter(pl.col("owner") == name).tail(1).row(0, named=True)
        b = buckets.filter(pl.col("owner") == name)
        mine = f.filter(pl.col("owner") == name)
        maker = mine.filter(pl.col("liquidity") == "maker")
        taker = mine.filter(pl.col("liquidity") == "taker")
        spread_maker = float(np.nansum(maker["spread_capture"].to_numpy()))
        spread_taker = float(np.nansum(taker["spread_capture"].to_numpy()))
        spread = float(b["spread_capture"].sum())
        inventory = float(b["inventory"].sum())
        fees = float(b["fees"].sum())
        notional = float(np.sum(np.abs(mine["qty"].to_numpy()) * mine["price"].to_numpy()))
        rows.append({
            "owner": name,
            "pnl": final["pnl"],
            "spread_capture": spread,
            "spread_maker": spread_maker,
            "spread_taker": spread_taker,
            "inventory": inventory,
            "fees": fees,
            "residual": final["pnl"] - (spread + inventory - fees),
            "fills": len(mine),
            "maker_fills": len(maker),
            "taker_fills": len(taker),
            "base_volume": float(np.sum(np.abs(mine["qty"].to_numpy()))),
            "quote_volume": notional,
            "edge_bps": spread / notional * 1e4 if notional else 0.0,
            "position": final["position"],
            "mark": final["mark"],
        })
    return pl.DataFrame(rows)
