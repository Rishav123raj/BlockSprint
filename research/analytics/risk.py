"""Risk (P5.8): risk-adjusted returns, drawdown, inventory, time at limits and turnover.

    from research.analytics import risk
    risk.summary(run)                    # one row per strategy: everything below
    risk.ratios(run, "1m")               # Sharpe, Sortino from the PnL per bucket
    risk.drawdown(run)                   # the largest fall of the mark-to-mid PnL from a previous high
    risk.inventory(run)                  # time-weighted position distribution
    risk.limits(run, position_limit=0.01)   # time at the position and open-order limits, limit rejects
    risk.turnover(run)                   # volume, per hour, inventory turnover and holding time

Everything is measured from the end of the warm-up to the end of the run, in quote units unless said.

* **Sharpe / Sortino** use the PnL of each time bucket (research.analytics.pnl.pnl_buckets, default 1 minute):
  mean / standard deviation, and mean / downside deviation (the root mean square of the negative buckets,
  counting the others as 0), annualized by the square root of the buckets in a year (365 days: crypto
  trades all the time). They are ratios of PnL, so they need no capital figure; with few buckets they mean
  little (`buckets` says how many).
* **Drawdown** is on the engine's own mark-to-mid PnL at every sample (positions.csv): the largest fall from
  a running high, when the high was, the low after it, and when (if ever) the PnL got back to the high.
* **Inventory** is the exact position path from the fills, weighted by time: mean, mean absolute, standard
  deviation, quantiles, the time flat / long / short, and the mean absolute notional at the mid.
* **Limits:** the time the position is at a limit (|position| >= limit; the limit is `position_limit` or the
  strategy's `max_position` parameter if the config gives one), the share of samples with as many open
  orders as `limits.open_orders`, and rejects for the open-order and rate limits.
* **Turnover:** traded volume in base and quote units, per hour, inventory turnover (base volume / mean
  absolute position) and the mean holding time (mean absolute position / base volume per second x 2: a
  unit bought is held until a unit is sold).
"""

from __future__ import annotations

from datetime import timedelta
from typing import Any

import numpy as np

from research.analytics.market import MarketData, run_window, table
from research.analytics.pnl import fills as _fills
from research.analytics.pnl import parse_every, pnl_buckets
from research.analytics.tca import _EPS

YEAR_NS = 365 * 86_400 * 1_000_000_000


def _owners(run: Any) -> list[str]:
    return list(dict.fromkeys(table(run, "positions")["owner"].to_list()))


def ratios(run: Any, every: str | int | timedelta = "1m", market: MarketData | None = None):
    """Per strategy: buckets, mean and std of the bucket PnL, sharpe, sortino (annualized), hit_rate (share of
    buckets with a gain), best and worst bucket."""
    import polars as pl

    every_ns = parse_every(every)
    b = pnl_buckets(run, every_ns, market)
    rows = []
    per_year = YEAR_NS / every_ns
    for name in _owners(run):
        mine = b.filter(pl.col("owner") == name)
        # A last, shorter bucket would weigh like a full one: leave it out
        mine = mine.filter((pl.col("end_ns") - pl.col("start_ns")) == every_ns) if mine.height > 1 else mine
        r = mine["pnl"].to_numpy()
        n = len(r)
        mean = float(r.mean()) if n else float("nan")
        std = float(r.std(ddof=1)) if n > 1 else float("nan")
        down = float(np.sqrt(np.mean(np.minimum(r, 0.0) ** 2))) if n else float("nan")
        rows.append({
            "owner": name, "buckets": n, "bucket": every if isinstance(every, str) else f"{every_ns}ns",
            "mean": mean, "std": std,
            "sharpe": mean / std * np.sqrt(per_year) if std and std > 0 else float("nan"),
            "sortino": mean / down * np.sqrt(per_year) if down > 0 else float("nan"),
            "hit_rate": float((r > 0).mean()) if n else float("nan"),
            "best": float(r.max()) if n else float("nan"),
            "worst": float(r.min()) if n else float("nan"),
        })
    return pl.DataFrame(rows)


def drawdown_series(run: Any):
    """The engine's mark-to-mid PnL at every sample from the end of the warm-up, with its running high and
    the drawdown below it: owner, ts_ns, pnl, high, drawdown."""
    import polars as pl

    _, trading, _ = run_window(run)
    pos = table(run, "positions").filter(pl.col("ts_ns") >= trading).select("owner", "ts_ns", "pnl")
    return pos.with_columns(pl.col("pnl").cum_max().over("owner").alias("high")).with_columns(
        (pl.col("high") - pl.col("pnl")).alias("drawdown"))


def drawdown(run: Any):
    """Per strategy: max_drawdown (quote), peak_ns and peak_pnl (the high before it), trough_ns and trough_pnl,
    recovered_ns (the first sample back at the high; null if never), duration_s (peak to recovery, or to the
    end), and the final pnl."""
    import polars as pl

    s = drawdown_series(run)
    rows = []
    for name in _owners(run):
        m = s.filter(pl.col("owner") == name)
        ts, pnl, high, dd = (m[c].to_numpy() for c in ("ts_ns", "pnl", "high", "drawdown"))
        if not len(ts):
            continue
        k = int(np.argmax(dd))  # the trough
        peak_val = high[k]
        j = int(np.argmax(pnl[: k + 1] >= peak_val - _EPS)) if k else 0  # the first time at that high
        after = np.nonzero(pnl[k:] >= peak_val - _EPS)[0]
        recovered = int(ts[k + after[0]]) if dd[k] > 0 and len(after) else None
        end = recovered if recovered is not None else int(ts[-1])
        rows.append({
            "owner": name, "max_drawdown": float(dd[k]), "peak_ns": int(ts[j]), "peak_pnl": float(peak_val),
            "trough_ns": int(ts[k]), "trough_pnl": float(pnl[k]), "recovered_ns": recovered,
            "duration_s": (end - int(ts[j])) / 1e9 if dd[k] > 0 else 0.0, "pnl": float(pnl[-1]),
        })
    return pl.DataFrame(rows, schema_overrides={"recovered_ns": pl.Int64})


def _position_path(f, name: str, start: int, end: int):
    """(times, positions): the position from times[i] to times[i + 1], from start to end."""
    import polars as pl

    mine = f.filter(pl.col("owner") == name)
    t = mine["exchange_ns"].to_numpy()
    pos = np.cumsum(mine["qty"].to_numpy())
    times = np.concatenate([[start], np.clip(t, start, end), [end]]).astype(np.int64)
    values = np.concatenate([[0.0], pos])
    return times, values


def _weighted_quantile(values: np.ndarray, weights: np.ndarray, q: float) -> float:
    order = np.argsort(values, kind="stable")
    v, w = values[order], weights[order]
    c = np.cumsum(w)
    if c[-1] <= 0:
        return float("nan")
    return float(v[np.searchsorted(c, q * c[-1], side="left").clip(0, len(v) - 1)])


def inventory(run: Any, market: MarketData | None = None):
    """Per strategy, the position weighted by time from the end of the warm-up: mean, mean_abs, std, min, max,
    p05, p50, p95, flat / long / short (shares of time), mean_abs_notional (|position| x mid), final."""
    import polars as pl

    market = market or MarketData.for_run(run)
    _, start, end = run_window(run)
    f = _fills(run, market)
    rows = []
    for name in _owners(run):
        times, values = _position_path(f, name, start, end)
        w = np.diff(times).astype(np.float64)
        total = w.sum()
        if total <= 0:
            continue
        mean = float((values * w).sum() / total)
        mids = market.mid(times[:-1])
        notional = np.abs(values) * np.nan_to_num(mids)
        rows.append({
            "owner": name,
            "mean": mean,
            "mean_abs": float((np.abs(values) * w).sum() / total),
            "std": float(np.sqrt(((values - mean) ** 2 * w).sum() / total)),
            "min": float(values.min()), "max": float(values.max()),
            "p05": _weighted_quantile(values, w, 0.05),
            "p50": _weighted_quantile(values, w, 0.50),
            "p95": _weighted_quantile(values, w, 0.95),
            "flat": float(w[np.abs(values) <= _EPS].sum() / total),
            "long": float(w[values > _EPS].sum() / total),
            "short": float(w[values < -_EPS].sum() / total),
            "mean_abs_notional": float((notional * w).sum() / total),
            "final": float(values[-1]),
        })
    return pl.DataFrame(rows)


def limits(run: Any, position_limit: float | None = None, market: MarketData | None = None):
    """Per strategy: position_limit (given, else the strategy's max_position parameter, else null),
    time_at_position_limit (share of time with |position| >= it), max_abs_position, open_orders_limit
    (limits.open_orders), samples_at_open_orders_limit (share of samples),
    rejects_open_orders (orders refused for the open-order limit), rejects_rate_limit (every request refused
    for the order-count or request-weight limits: orders, and cancels / amends / replaces) and rejects_risk_guard
    (every request the strategy's own RiskGuard refused: risk_position / notional / open_orders / killed) and
    rejects_gap_halt (every request refused while its gap policy halted trading)."""
    import polars as pl

    from research.analytics.tca import rejects

    market = market or MarketData.for_run(run)
    cfg = run.meta.get("config", {})
    if position_limit is None:
        p = cfg.get("strategy", {}).get("params", {}).get("max_position")
        position_limit = float(p) if p is not None else None
    oo_limit = cfg.get("limits", {}).get("open_orders")
    _, start, end = run_window(run)
    f = _fills(run, market)
    pos = table(run, "positions").filter(pl.col("ts_ns") >= start)
    rj = rejects(run)
    rows = []
    for name in _owners(run):
        times, values = _position_path(f, name, start, end)
        w = np.diff(times).astype(np.float64)
        total = w.sum()
        at = (float(w[np.abs(values) >= position_limit - _EPS].sum() / total)
              if position_limit is not None and total > 0 else None)
        mine = pos.filter(pl.col("owner") == name)
        oo = mine["open_orders"].to_numpy()
        mine_rj = rj.filter(pl.col("owner") == name)
        reasons = dict(mine_rj.filter(pl.col("kind") == "order").select("reason", "count").iter_rows())
        rate = int(mine_rj.filter(pl.col("reason") == "rate_limit")["count"].sum())
        guard = int(mine_rj.filter(pl.col("reason").str.starts_with("risk_"))["count"].sum())
        halted = int(mine_rj.filter(pl.col("reason") == "gap_halted")["count"].sum())
        rows.append({
            "owner": name, "position_limit": position_limit, "time_at_position_limit": at,
            "max_abs_position": float(np.abs(values).max()),
            "open_orders_limit": oo_limit,
            "samples_at_open_orders_limit": float((oo >= oo_limit).mean()) if oo_limit and len(oo) else None,
            "rejects_open_orders": int(reasons.get("max_open_orders", 0)),
            "rejects_rate_limit": rate,
            "rejects_risk_guard": guard,
            "rejects_gap_halt": halted,
        })
    return pl.DataFrame(rows, schema_overrides={"position_limit": pl.Float64, "time_at_position_limit": pl.Float64,
                                                "open_orders_limit": pl.Int64,
                                                "samples_at_open_orders_limit": pl.Float64})


def turnover(run: Any, market: MarketData | None = None, inventory_df=None):
    """Per strategy: base_volume, quote_volume, hours (from the end of the warm-up), quote_volume_per_hour,
    inventory_turnover (base volume / time-weighted mean |position|) and holding_time_s (mean |position| /
    (base volume / 2 per second))."""
    import polars as pl

    market = market or MarketData.for_run(run)
    _, start, end = run_window(run)
    hours = (end - start) / 3.6e12
    f = _fills(run, market)
    inv = inventory_df if inventory_df is not None else inventory(run, market)
    mean_abs = dict(inv.select("owner", "mean_abs").iter_rows()) if inv.height else {}
    rows = []
    for name in _owners(run):
        mine = f.filter(pl.col("owner") == name)
        base = float(mine["qty"].abs().sum())
        quote = float((mine["qty"].abs() * mine["price"]).sum())
        ma = mean_abs.get(name, 0.0)
        per_s = base / 2 / (hours * 3600) if hours > 0 else 0.0
        rows.append({
            "owner": name, "base_volume": base, "quote_volume": quote, "hours": hours,
            "quote_volume_per_hour": quote / hours if hours > 0 else float("nan"),
            "inventory_turnover": base / ma if ma > _EPS else float("nan"),
            "holding_time_s": ma / per_s if per_s > 0 else float("nan"),
        })
    return pl.DataFrame(rows)


def summary(run: Any, every: str | int | timedelta = "1m", position_limit: float | None = None,
            market: MarketData | None = None):
    """One row per strategy: ratios, drawdown, inventory (prefixed inv_), limits and turnover together."""
    market = market or MarketData.for_run(run)
    inv = inventory(run, market)
    parts = [
        ratios(run, every, market).drop("bucket"),
        drawdown(run).select("owner", "max_drawdown", "duration_s", "pnl"),
        inv.select("owner", *[c for c in inv.columns if c != "owner"]).rename(
            {c: f"inv_{c}" for c in inv.columns if c != "owner"}),
        limits(run, position_limit, market).drop("max_abs_position"),
        turnover(run, market, inv),
    ]
    out = parts[0]
    for p in parts[1:]:
        out = out.join(p, on="owner", how="left")
    return out
