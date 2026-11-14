"""Parity report (P7.5): a paper session against its simulator replays (docs/parity_protocol.md).

Both sides are measured the same way, against the same market: the session's recording (the replay's .bse
file), so a difference in a measure is a difference in what happened to the orders, not in the yardstick.

Per side (paper, and each replay seed):

* ``fill_ratio``: quantity filled / quantity offered (every new order's and every replace's quantity);
* ``fills``, ``volume``, ``maker_share``;
* ``time_to_fill_s``: per fill, from the order's last placement (its new order or replace) to the fill: median
  and p90;
* ``markout_<h>_ticks``: per fill, the data's mid ``h`` after the fill minus the fill price, signed by the side
  (positive: the price moved in the fill's favour), averaged over fills weighted by quantity, in ticks;
* paper fills are timed at the data trade that made them (``data_time_lag_ms``: how much later the server stamped
  them), so both sides are read against the same moment of the recording;
* ``spread_ticks``: per fill, the mid at the fill minus its price, signed by the side: what a maker fill earned
  against the mid at that moment (negative for a taker paying the spread), quantity-weighted, in ticks;
* PnL decomposition in quote units, marked at the data's mid at the end: ``spread_pnl`` (the sum of each fill's
  edge against the mid at its time), ``inventory_pnl`` (what the position gained or lost as the mid moved),
  ``fees``, and ``pnl`` = spread + inventory - fees.

The comparison takes the mean over seeds for the simulator and checks Phase 7's acceptance: the fill ratio
within ±15% relative, and the 1 s markout mean within the simulator's own noise: the range of the 1 s markout
across the replay seeds (at least 1 tick, which is also the bound with a single seed).

Usage::

    uv run python -m research.analytics.parity data/paper/2026-11-14_quoter_btc_1 data/paper/2026-11-14_quoter_btc_2
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

import blocksprint_sim as bs

SCALE = 1e8
HORIZONS_S = (1, 5, 30)
FILL_RATIO_TOL = 0.15  # relative
MARKOUT_FLOOR_TICKS = 1.0  # the markout tolerance is the seed range, never below this


@dataclass
class Side:
    """One run's orders and fills, in floats (quote and base units) and ns."""
    name: str
    requests: dict[str, np.ndarray]  # ts_ns, order_id (object), qty (offered: new orders and replaces)
    fills: dict[str, np.ndarray]     # ts_ns, order_id, sign (+1 buy, -1 sell), px, qty, fee, maker (bool)


def _arr(rows: list[tuple], names: tuple[str, ...], kinds: tuple[str, ...]) -> dict[str, np.ndarray]:
    cols = list(zip(*rows)) if rows else [[] for _ in names]
    return {n: np.asarray(c, dtype=k) for n, c, k in zip(names, cols, kinds)}


REQ = (("ts_ns", "order_id", "qty"), ("int64", "object", "float64"))
FILL = (("ts_ns", "order_id", "sign", "px", "qty", "fee", "maker"),
        ("int64", "object", "float64", "float64", "float64", "float64", "bool"))


def paper_side(session: Path) -> Side:
    from research.live.runner import iso_ns

    reqs, fills = [], []
    for line in (session / "paper" / "session.jsonl").read_text(encoding="utf-8").splitlines():
        d = json.loads(line)
        if d["kind"] == "request" and d.get("action") in ("submit", "replace"):
            reqs.append((d["t"], d["order_id"], float(d["quantity"])))
        elif d["kind"] == "report" and d.get("type") == "fill":
            fills.append((iso_ns(d["timestamp"]), d["order_id"], 1.0 if d["side"] == "buy" else -1.0,
                          float(d["last_px"]), float(d["last_qty"]), float(d.get("fee", 0)),
                          d.get("liquidity") == "maker"))
    return Side("paper", _arr(reqs, *REQ), _arr(fills, *FILL))


def sim_side(run_dir: Path, name: str) -> Side:
    import polars as pl

    o = pl.read_parquet(run_dir / "orders.parquet").filter(pl.col("request").is_in(["new", "replace"]))
    r = pl.read_parquet(run_dir / "exec_reports.parquet").filter(pl.col("type") == "fill")
    ns = lambda c: pl.col(c).cast(pl.Int64)  # noqa: E731  UTC datetimes in ns
    o = o.select(ns("sent_ns").alias("ts_ns"), "order_id", pl.col("quantity").alias("qty"))
    r = r.select(ns("exchange_ns").alias("ts_ns"), "order_id",
                 pl.when(pl.col("side") == "buy").then(1.0).otherwise(-1.0).alias("sign"),
                 pl.col("last_px").alias("px"), pl.col("last_qty").alias("qty"), pl.col("fee").fill_null(0.0),
                 (pl.col("liquidity") == "maker").alias("maker"))
    reqs = {"ts_ns": o["ts_ns"].to_numpy(), "order_id": o["order_id"].to_numpy().astype(object),
            "qty": o["qty"].to_numpy().astype(np.float64)}
    fills = {c: r[c].to_numpy() for c in FILL[0]}
    fills["order_id"] = fills["order_id"].astype(object)
    return Side(name, reqs, fills)


class Mids:
    """The data's mid over time, from the session's .bse file."""

    def __init__(self, bse: str | Path, from_ns: int, to_ns: int):
        b = bs.data_bbo(str(bse), int(from_ns), int(to_ns))
        ok = (b["bid"] > 0) & (b["ask"] > 0)
        self.ts = b["ts_ns"][ok].astype(np.int64)
        self.mid = (b["bid"][ok].astype(np.float64) + b["ask"][ok].astype(np.float64)) / 2 / SCALE

    def at(self, ts_ns: np.ndarray, before: bool = False) -> np.ndarray:
        """The mid as of each time; before: from the updates strictly before it (the market a fill met, not the
        update that reflects the fill's own trade, which carries the same time)."""
        i = np.searchsorted(self.ts, np.asarray(ts_ns, dtype=np.int64), side="left" if before else "right") - 1
        out = np.full(len(i), np.nan)
        good = i >= 0
        out[good] = self.mid[i[good]]
        return out


def _wavg(x: np.ndarray, w: np.ndarray) -> float | None:
    ok = ~np.isnan(x)
    return float(np.average(x[ok], weights=w[ok])) if ok.any() and w[ok].sum() > 0 else None


def _data_trade_time(trades: dict[str, np.ndarray], t: int, px: float, sign: float) -> int:
    """The time of the latest data trade at px whose aggressor is on the other side (a buy fill meets a seller),
    within the second up to t; 0 if none."""
    lo = np.searchsorted(trades["ts"], t - 1_000_000_000, side="left")
    hi = np.searchsorted(trades["ts"], t, side="right")
    want_sell = sign > 0
    for i in range(hi - 1, lo - 1, -1):
        if trades["sell"][i] == want_sell and abs(trades["px"][i] - px) < 1e-9:
            return int(trades["ts"][i])
    return 0


def data_trades(bse: str | Path, from_ns: int, to_ns: int) -> dict[str, np.ndarray]:
    t = bs.data_trades(str(bse), int(from_ns), int(to_ns))
    return {"ts": t["ts_ns"].astype(np.int64), "px": t["px"].astype(np.float64) / SCALE, "sell": t["side"] == 1}


def measure(side: Side, mids: Mids, tick: float, end_ns: int,
            trades: dict[str, np.ndarray] | None = None) -> dict[str, Any]:
    """trades: the data's trades, to time the fills at them (paper fills; the simulator's are already)."""
    f, q = side.fills, side.requests
    n = len(f["ts_ns"])
    offered = float(q["qty"].sum())
    volume = float(f["qty"].sum())
    out: dict[str, Any] = {"run": side.name, "fills": n, "volume": round(volume, 8),
                           "offered": round(offered, 8), "fill_ratio": volume / offered if offered else None,
                           "maker_share": float(f["maker"].mean()) if n else None}
    # Time to fill: from the order's last placement before the fill
    waits = []
    by_id: dict[str, list[int]] = {}
    for t, oid in zip(q["ts_ns"], q["order_id"]):
        by_id.setdefault(oid, []).append(int(t))
    for t, oid in zip(f["ts_ns"], f["order_id"]):
        placed = [p for p in by_id.get(oid, []) if p <= t]
        if placed:
            waits.append((int(t) - max(placed)) / 1e9)
    out["time_to_fill_s"] = ({"median": float(np.median(waits)), "p90": float(np.percentile(waits, 90))}
                             if waits else None)
    out["data_time_lag_ms"] = None
    if trades is not None and n:
        # A paper fill is stamped when the server applied the trade, a few ms after the recorder stamped the same
        # Binance trade; a depth update in between already shows the move. Measure a paper fill at the data
        # trade that made it: the latest one at its price, from the other side, within the second before.
        aligned = np.array([_data_trade_time(trades, int(t), px, sg) for t, px, sg in zip(f["ts_ns"], f["px"], f["sign"])])
        found = aligned > 0
        if found.any():
            out["data_time_lag_ms"] = float(np.median((f["ts_ns"][found] - aligned[found]) / 1e6))
        out["fills_matched_to_data"] = float(found.mean())
        f = dict(f, ts_ns=np.where(found, aligned, f["ts_ns"]))
    mid_now = mids.at(f["ts_ns"], before=True)
    edge = f["sign"] * (mid_now - f["px"])  # quote per base unit
    out["spread_ticks"] = _wavg(edge / tick, f["qty"])
    out["spread_bps"] = _wavg(edge / f["px"] * 1e4, f["qty"])
    for h in HORIZONS_S:
        later = mids.at(f["ts_ns"] + h * 1_000_000_000)
        out[f"markout_{h}s_ticks"] = _wavg(f["sign"] * (later - f["px"]) / tick, f["qty"])
        out[f"markout_{h}s_bps"] = _wavg(f["sign"] * (later - f["px"]) / f["px"] * 1e4, f["qty"])
    # PnL at the data's mid at the end
    end_mid = float(mids.at(np.array([end_ns]))[0])
    position = float((f["sign"] * f["qty"]).sum())
    cash = float((-f["sign"] * f["px"] * f["qty"]).sum())
    fees = float(f["fee"].sum())
    spread_pnl = float(np.nansum(edge * f["qty"]))
    gross = cash + position * end_mid
    out.update({"position": round(position, 8), "fees": fees, "spread_pnl": spread_pnl,
                "inventory_pnl": gross - spread_pnl, "pnl": gross - fees})
    return out


def session_report(session: str | Path, sim_dir: str | Path | None = None) -> dict[str, Any]:
    """Paper and every replay seed of a session, measured alike, and the acceptance checks. sim_dir: the replay
    to compare (default ``<session>/sim``)."""
    session = Path(session)
    sim_dir = Path(sim_dir) if sim_dir else session / "sim"
    meta = json.loads((session / "paper" / "session_meta.json").read_text(encoding="utf-8"))
    bse = next((session / "packed" / "session").glob("*.bse"))
    runs = sorted(p for p in sim_dir.glob("seed_*") if (p / "exec_reports.parquet").exists())
    if not runs:
        raise FileNotFoundError(f"{session}: no replay (run paper_session replay first)")
    run_meta = json.loads((runs[0] / "run_meta.json").read_text(encoding="utf-8"))
    tick = float(run_meta["data_header"]["tick_size"])
    start, end = int(meta["trading_ns"]), int(meta["end_ns"])
    mids = Mids(bse, start - 60 * 10**9, end + (max(HORIZONS_S) + 5) * 10**9)
    trades = data_trades(bse, start - 60 * 10**9, end + 60 * 10**9)
    paper = measure(paper_side(session), mids, tick, end, trades)
    sims = [measure(sim_side(p, p.name), mids, tick, end) for p in runs]
    keys = ["fills", "volume", "fill_ratio", "maker_share", "spread_ticks", "spread_bps", "spread_pnl", "inventory_pnl",
            "fees", "pnl"] + [f"markout_{h}s_{u}" for h in HORIZONS_S for u in ("ticks", "bps")]
    sim_mean = {k: (float(np.mean([s[k] for s in sims if s[k] is not None]))
                    if any(s[k] is not None for s in sims) else None) for k in keys}
    sim_mean["time_to_fill_s"] = {"median": float(np.mean([s["time_to_fill_s"]["median"] for s in sims
                                                            if s["time_to_fill_s"]]))} if sims else None
    checks = {}
    if paper["fill_ratio"] and sim_mean["fill_ratio"] is not None:
        rel = sim_mean["fill_ratio"] / paper["fill_ratio"] - 1
        checks["fill_ratio_rel"] = {"value": rel, "pass": abs(rel) <= FILL_RATIO_TOL}
    if paper["markout_1s_ticks"] is not None and sim_mean["markout_1s_ticks"] is not None:
        d = sim_mean["markout_1s_ticks"] - paper["markout_1s_ticks"]
        # The tolerance is how far apart the seeds themselves are: the simulator's own noise at this sample size
        ms = [s["markout_1s_ticks"] for s in sims if s["markout_1s_ticks"] is not None]
        seed_range = (max(ms) - min(ms)) if len(ms) > 1 else None
        tol = max(seed_range or 0.0, MARKOUT_FLOOR_TICKS)
        checks["markout_1s_diff_ticks"] = {"value": d, "tolerance": tol, "seed_range": seed_range,
                                           "pass": abs(d) <= tol}
    protocol = json.loads((session / "protocol.json").read_text(encoding="utf-8"))
    return {"session": session.name, "symbol": protocol["symbol"], "strategy": protocol["strategy"],
            "queue_model": protocol["queue_model"], "minutes": (end - start) / 6e10, "tick": tick,
            "paper": paper, "sim": sims, "sim_mean": sim_mean, "checks": checks,
            "measured_latency": meta.get("measured_latency")}


def summary(reports: list[dict[str, Any]]) -> dict[str, Any]:
    """Across sessions: every check must pass in every session (the acceptance asks for at least three)."""
    passed = {name: all(r["checks"].get(name, {}).get("pass", False) for r in reports)
              for name in ("fill_ratio_rel", "markout_1s_diff_ticks")}
    return {"sessions": len(reports), "passed": passed, "enough_sessions": len(reports) >= 3,
            "accepted": len(reports) >= 3 and all(passed.values())}


def _fmt(v: Any, digits: int = 3) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.{digits}f}"
    return str(v)


def render(reports: list[dict[str, Any]]) -> str:
    """The reports as Markdown tables."""
    lines = []
    for r in reports:
        lines += [f"### {r['session']} ({r['symbol']}, {r['minutes']:.1f} min, {r['queue_model']})", "",
                  "| | fills | volume | fill ratio | maker | ttf median s | spread ticks | markout 1s ticks | 5s | 30s | "
                  "markout 1s bps | spread PnL | inventory PnL | fees | PnL |",
                  "| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for row in [r["paper"]] + r["sim"]:
            ttf = row["time_to_fill_s"]["median"] if row["time_to_fill_s"] else None
            lines.append("| " + " | ".join([row["run"], _fmt(row["fills"]), _fmt(row["volume"], 4), _fmt(row["fill_ratio"]),
                                            _fmt(row["maker_share"], 2), _fmt(ttf, 2), _fmt(row["spread_ticks"], 2),
                                            *[_fmt(row[f"markout_{h}s_ticks"], 2) for h in HORIZONS_S],
                                            _fmt(row["markout_1s_bps"], 3),
                                            _fmt(row["spread_pnl"], 2), _fmt(row["inventory_pnl"], 2),
                                            _fmt(row["fees"], 2), _fmt(row["pnl"], 2)]) + " |")
        checks = ", ".join(f"{k} {_fmt(v['value'])}" + (f" within {_fmt(v['tolerance'], 1)}" if "tolerance" in v else "")
                       + f" ({'pass' if v['pass'] else 'FAIL'})" for k, v in r["checks"].items())
        lines += ["", f"Checks: {checks}", ""]
    s = summary(reports)
    lines.append(f"**{s['sessions']} session(s)**: fill ratio {'pass' if s['passed']['fill_ratio_rel'] else 'FAIL'}, "
                 f"1 s markout {'pass' if s['passed']['markout_1s_diff_ticks'] else 'FAIL'}; "
                 f"acceptance {'met' if s['accepted'] else 'not met'}"
                 + ("" if s["enough_sessions"] else " (needs at least 3 sessions)"))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Paper-vs-simulator parity report (P7.5)")
    p.add_argument("sessions", nargs="+", help="session directories (paper_session run + replay)")
    p.add_argument("--json", help="also write the reports here")
    args = p.parse_args(argv)
    reports = [session_report(s) for s in args.sessions]
    for r in reports:
        (Path(args.sessions[reports.index(r)]) / "parity.json").write_text(json.dumps(r, indent=2), encoding="utf-8")
    if args.json:
        Path(args.json).write_text(json.dumps({"reports": reports, "summary": summary(reports)}, indent=2),
                                   encoding="utf-8")
    print(render(reports))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
