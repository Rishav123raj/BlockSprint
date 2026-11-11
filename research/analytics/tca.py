"""Transaction cost analysis (P5.5): what trading cost against the market, order by order and fill by fill.

    from research.analytics import tca
    tca.summary(run)                 # one row per strategy
    tca.orders(run)                  # one row per order placement, with its arrival mid and shortfall
    tca.fill_costs(run, "5s")        # each fill's effective and realized spread and price impact
    tca.slippage(run)                # each strategy and side against the market's VWAP and the mid's TWAP
    tca.rejects(run)                 # refused orders and requests by reason

Costs are signed so that **positive is a cost** to the strategy (paid), negative a gain, in basis points of
the mid unless the column says quote units. s is +1 for a buy and -1 for a sell; m(t) is the data's mid
(research.analytics.market).

* **A placement** is one version of an order: a NEW, or a REPLACE (which keeps the order ID; the old version
  ends `cancelled` / `replaced`). Its reports are those from its ack (or rejection) to the next version's.
* **Implementation shortfall** of a placement: s x (average fill price - m(sent)) on what filled, m(sent) being
  the mid when the strategy sent it (the arrival price). The **opportunity cost** of what didn't fill:
  s x (m(end) - m(sent)) x unfilled, at the placement's end (cancel or replace; the run's end if still open).
* **Effective spread** of a fill: 2s(price - m(t)). **Realized spread** at horizon h: 2s(price - m(t + h)), what
  the fill cost once the market has moved on. **Price impact**: 2s(m(t + h) - m(t)) = effective - realized.
  The mid at a fill is the one after the data batch that caused it (as in P5.4), so a passive fill that came
  because the market traded through its price shows an effective spread near zero or positive, not minus
  half the spread. For a maker a negative impact is adverse selection: after it buys, the market falls.
* **Slippage** per strategy and side: s x (its average fill price - benchmark) over its execution interval,
  from the first placement sent on that side to its last fill, against the market's VWAP (the data's trades)
  and the mid's TWAP over the same interval.
* **Fill ratio:** filled / placed quantity. **Time to fill:** from a placement's arrival at the exchange to its
  first fill (and to its last, when it filled completely). **Cancel / fill:** cancellations (by the strategy
  or by a replace) per fill report. **Order / trade:** requests sent per fill report.
"""

from __future__ import annotations

from datetime import timedelta
from typing import Any

import numpy as np

from research.analytics.market import MarketData, run_window, table
from research.analytics.pnl import fills as _fills
from research.analytics.pnl import parse_every

_EPS = 1e-12  # quantities are multiples of 1e-8: anything smaller is zero


def orders(run: Any, market: MarketData | None = None):
    """One row per placement: owner, order_id, version (1 = the NEW, then one per REPLACE), request,
    side, order_type, price, quantity, sent_ns, arrive_ns, status (filled / cancelled / rejected / open),
    end_reason (the cancel's or rejection's reason), end_ns, fills, filled_qty, avg_px, maker_qty, fees,
    first_fill_ns, last_fill_ns, time_to_first_fill_ms, time_to_fill_ms (complete fills only), mid_sent,
    mid_arrive, shortfall (quote) and shortfall_bps (on the filled part), opportunity_cost (quote)."""
    import polars as pl

    market = market or MarketData.for_run(run)
    _, _, w_end = run_window(run)
    rep = table(run, "exec_reports").with_row_index("_row")
    rep = rep.with_columns(
        pl.col("type").is_in(["ack", "rejected"]).cast(pl.Int64).cum_sum().over("owner", "order_id")
        .clip(lower_bound=1).alias("version"))
    is_fill = pl.col("type") == "fill"
    notional = pl.col("last_px") * pl.col("last_qty")
    keys = ["owner", "order_id", "version"]
    agg = rep.group_by(keys, maintain_order=True).agg(
        pl.col("exchange_ns").first().alias("arrive_ns"),
        pl.col("type").first().alias("_first_type"),
        pl.col("reason").first().alias("_first_reason"),
        pl.col("side").first(),
        pl.col("order_type").first(),
        pl.col("price").first(),
        pl.col("quantity").first(),
        pl.col("type").last().alias("_last_type"),
        pl.col("reason").last().alias("_last_reason"),
        pl.col("exchange_ns").last().alias("_last_ns"),
        pl.col("leaves_qty").last().alias("_leaves"),
        is_fill.sum().cast(pl.Int64).alias("fills"),
        pl.col("last_qty").filter(is_fill).sum().alias("filled_qty"),
        notional.filter(is_fill).sum().alias("_notional"),
        pl.col("last_qty").filter(is_fill & (pl.col("liquidity") == "maker")).sum().alias("maker_qty"),
        pl.col("fee").filter(is_fill).sum().alias("fees"),
        pl.col("exchange_ns").filter(is_fill).first().alias("first_fill_ns"),
        pl.col("exchange_ns").filter(is_fill).last().alias("last_fill_ns"),
    )
    status = (pl.when(pl.col("_first_type") == "rejected").then(pl.lit("rejected"))
              .when((pl.col("_last_type") == "fill") & (pl.col("_leaves") <= _EPS)).then(pl.lit("filled"))
              .when(pl.col("_last_type") == "cancelled").then(pl.lit("cancelled"))
              .otherwise(pl.lit("open")))
    agg = agg.with_columns(status.alias("status")).with_columns(
        pl.when(pl.col("status") == "rejected").then(pl.col("_first_reason"))
        .when(pl.col("status") == "cancelled").then(pl.col("_last_reason"))
        .otherwise(pl.lit(None, pl.String)).alias("end_reason"),
        pl.when(pl.col("status") == "open").then(pl.lit(w_end, pl.Int64)).otherwise(pl.col("_last_ns"))
        .alias("end_ns"),
        pl.when(pl.col("filled_qty") > _EPS).then(pl.col("_notional") / pl.col("filled_qty")).alias("avg_px"),
    )

    # The request that made each placement: it arrived when the placement started
    req = (table(run, "orders").filter(pl.col("request").is_in(["new", "replace"]))
           .select("owner", "order_id", "arrive_ns", "sent_ns", "request")
           .unique(["owner", "order_id", "arrive_ns"], keep="first", maintain_order=True))
    agg = agg.join(req, on=["owner", "order_id", "arrive_ns"], how="left")

    sign = np.where(agg["side"].to_numpy() == "buy", 1.0, -1.0)
    sent = agg["sent_ns"].fill_null(agg["arrive_ns"]).to_numpy().astype(np.int64)  # ns: never via float
    mid_sent = market.mid(sent)
    mid_arrive = market.mid(agg["arrive_ns"].to_numpy(), "left")
    mid_end = market.mid(agg["end_ns"].to_numpy())
    filled = agg["filled_qty"].to_numpy()
    avg = agg["avg_px"].fill_null(np.nan).to_numpy()
    unfilled = np.where(agg["status"].to_numpy() == "rejected", 0.0,
                        np.maximum(agg["quantity"].to_numpy() - filled, 0.0))
    has_fill = filled > _EPS
    shortfall = np.where(has_fill, sign * (avg - mid_sent) * filled, 0.0)
    shortfall_bps = np.where(has_fill, sign * (avg - mid_sent) / mid_sent * 1e4, np.nan)
    opportunity = sign * (mid_end - mid_sent) * unfilled
    arrive = agg["arrive_ns"].to_numpy()
    first_fill = agg["first_fill_ns"].fill_null(0).to_numpy()
    last_fill = agg["last_fill_ns"].fill_null(0).to_numpy()
    complete = agg["status"].to_numpy() == "filled"

    return agg.with_columns(
        pl.Series("sent_ns", sent),
        pl.Series("mid_sent", mid_sent),
        pl.Series("mid_arrive", mid_arrive),
        pl.Series("time_to_first_fill_ms", np.where(has_fill, (first_fill - arrive) / 1e6, np.nan)),
        pl.Series("time_to_fill_ms", np.where(complete, (last_fill - arrive) / 1e6, np.nan)),
        pl.Series("shortfall", shortfall),
        pl.Series("shortfall_bps", shortfall_bps),
        pl.Series("opportunity_cost", np.nan_to_num(opportunity) + 0.0),  # no -0.0
    ).with_columns(pl.col("time_to_first_fill_ms", "time_to_fill_ms", "shortfall_bps").fill_nan(None)).select(
        "owner", "order_id", "version", "request", "side", "order_type", "price", "quantity", "sent_ns",
        "arrive_ns", "status", "end_reason", "end_ns", "fills", "filled_qty", "avg_px", "maker_qty", "fees",
        "first_fill_ns", "last_fill_ns", "time_to_first_fill_ms", "time_to_fill_ms", "mid_sent", "mid_arrive",
        "shortfall", "shortfall_bps", "opportunity_cost")


def fill_costs(run: Any, horizon: str | int | timedelta = "5s", market: MarketData | None = None):
    """Each fill (research.analytics.pnl.fills) with mid_after (the mid at t + horizon) and, in basis points
    of the mid at the fill, effective_spread_bps, realized_spread_bps and price_impact_bps (cost positive),
    plus the same three in quote units (x quantity x mid / 1e4: effective_spread, realized_spread,
    price_impact)."""
    import polars as pl

    h = parse_every(horizon)
    market = market or MarketData.for_run(run)
    f = _fills(run, market)
    t = f["exchange_ns"].to_numpy()
    s = np.sign(f["qty"].to_numpy())
    px, mid = f["price"].to_numpy(), f["mid"].to_numpy()
    after = market.mid(t + h)
    eff = 2 * s * (px - mid) / mid * 1e4
    real = 2 * s * (px - after) / mid * 1e4
    impact = eff - real
    notional = np.abs(f["qty"].to_numpy()) * mid / 1e4
    return f.with_columns(
        pl.Series("mid_after", after),
        pl.Series("effective_spread_bps", eff),
        pl.Series("realized_spread_bps", real),
        pl.Series("price_impact_bps", impact),
        pl.Series("effective_spread", eff * notional),
        pl.Series("realized_spread", real * notional),
        pl.Series("price_impact", impact * notional),
    )


def slippage(run: Any, market: MarketData | None = None, placements=None):
    """One row per strategy and side that traded: start_ns, end_ns (its execution interval), filled_qty,
    avg_px, market_vwap, market_volume, mid_twap, participation (its share of the market's volume),
    vs_vwap_bps and vs_twap_bps (cost positive)."""
    import polars as pl

    market = market or MarketData.for_run(run)
    o = placements if placements is not None else orders(run, market)
    rows = []
    for (owner, side), g in o.filter(pl.col("filled_qty") > _EPS).group_by(["owner", "side"], maintain_order=True):
        start = int(g["sent_ns"].min())
        end = int(g["last_fill_ns"].max()) + 1
        qty = float(g["filled_qty"].sum())
        avg = float((g["avg_px"] * g["filled_qty"]).sum() / qty)
        vwap, volume = market.vwap(start, end)
        twap = market.twap(start, end)
        s = 1.0 if side == "buy" else -1.0
        rows.append({"owner": owner, "side": side, "start_ns": start, "end_ns": end, "filled_qty": qty,
                     "avg_px": avg, "market_vwap": vwap, "market_volume": volume, "mid_twap": twap,
                     "participation": qty / volume if volume else float("nan"),
                     "vs_vwap_bps": s * (avg - vwap) / vwap * 1e4, "vs_twap_bps": s * (avg - twap) / twap * 1e4})
    return pl.DataFrame(rows) if rows else pl.DataFrame()


def rejects(run: Any):
    """Refusals by owner, kind and reason, with a count: kind "order" for a NEW or REPLACE the exchange rejected
    (exec report `rejected`), else the refused request (cancel / amend / replace: request_rejects)."""
    import polars as pl

    rep = (table(run, "exec_reports").filter(pl.col("type") == "rejected")
           .select("owner", pl.lit("order").alias("kind"), "reason"))
    req = table(run, "request_rejects").select("owner", pl.col("request").alias("kind"), "reason")
    both = pl.concat([rep, req.cast(rep.schema)])
    return both.group_by(["owner", "kind", "reason"], maintain_order=True).len("count").sort(
        ["owner", "count"], descending=[False, True], maintain_order=True)


def summary(run: Any, horizon: str | int | timedelta = "5s", market: MarketData | None = None):
    """One row per strategy: placements, requests (new / replace / cancel / amend), rejected and refused counts,
    sent and filled quantity, fill_ratio (quantity), placements_filled (share with any fill), time to first
    fill (p50, mean, p90, ms), shortfall_bps (on filled notional), opportunity_cost (quote), effective /
    realized spread and price impact (bps, notional-weighted), cancels (by the strategy, by replaces),
    cancel_fill_ratio, order_to_trade."""
    import polars as pl

    market = market or MarketData.for_run(run)
    o = orders(run, market)
    fc = fill_costs(run, horizon, market)
    req = table(run, "orders")
    rep = table(run, "exec_reports")
    rj = rejects(run)
    owners = list(dict.fromkeys(table(run, "positions")["owner"].to_list()))
    rows = []
    for name in owners:
        po = o.filter(pl.col("owner") == name)
        pf = fc.filter(pl.col("owner") == name)
        pr = req.filter(pl.col("owner") == name)
        pe = rep.filter(pl.col("owner") == name)
        counts = dict(pr.group_by("request").len().iter_rows())
        n_req = pr.height
        fill_reports = int((pe["type"] == "fill").sum())
        cancelled = pe.filter(pl.col("type") == "cancelled")
        by_user = int((cancelled["reason"] == "user").sum())
        by_replace = int((cancelled["reason"] == "replaced").sum())
        sent_qty = float(po.filter(pl.col("status") != "rejected")["quantity"].sum())
        filled_qty = float(po["filled_qty"].sum())
        notional = float((po["avg_px"].fill_null(0) * po["filled_qty"]).sum())
        ttf = po["time_to_first_fill_ms"].drop_nulls().to_numpy()
        fill_notional = float((pf["qty"].abs() * pf["mid"]).sum())

        def weighted(col: str) -> float:
            return float(pf[col].sum() / fill_notional * 1e4) if fill_notional else float("nan")

        refused = rj.filter((pl.col("owner") == name) & (pl.col("kind") != "order"))
        placed = po.filter(pl.col("status") != "rejected").height
        rows.append({
            "owner": name,
            "placements": po.height,
            "new": counts.get("new", 0), "replace": counts.get("replace", 0),
            "cancel": counts.get("cancel", 0), "amend": counts.get("amend", 0),
            "rejected": int((po["status"] == "rejected").sum()),
            "refused_requests": int(refused["count"].sum()) if refused.height else 0,
            "sent_qty": sent_qty,
            "filled_qty": filled_qty,
            "fill_ratio": filled_qty / sent_qty if sent_qty else float("nan"),
            "placements_filled": float((po["filled_qty"] > _EPS).sum() / placed) if placed else float("nan"),
            "ttf_p50_ms": float(np.percentile(ttf, 50)) if len(ttf) else float("nan"),
            "ttf_mean_ms": float(ttf.mean()) if len(ttf) else float("nan"),
            "ttf_p90_ms": float(np.percentile(ttf, 90)) if len(ttf) else float("nan"),
            "shortfall_bps": float(po["shortfall"].sum() / notional * 1e4) if notional else float("nan"),
            "opportunity_cost": float(po["opportunity_cost"].sum()),
            "effective_spread_bps": weighted("effective_spread"),
            "realized_spread_bps": weighted("realized_spread"),
            "price_impact_bps": weighted("price_impact"),
            "fills": fill_reports,
            "cancels_user": by_user,
            "cancels_replace": by_replace,
            "cancel_fill_ratio": (by_user + by_replace) / fill_reports if fill_reports else float("nan"),
            "order_to_trade": n_req / fill_reports if fill_reports else float("nan"),
        })
    return pl.DataFrame(rows)
