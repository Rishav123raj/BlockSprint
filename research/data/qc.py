"""QC suite (P2.10) for normalized partitions: one qc.json and qc.html per symbol-date.

Checks:

* **Snapshot reconstruction:** every REST snapshot taken while the book was synced is compared with the
  reference book (refbook.py) rebuilt from the book stream, top 20 levels per side, exactly. A snapshot's
  ``lastUpdateId`` L usually falls inside a depth event's update range [U, u]. The book state at L is then
  known for every level except those that event touched, so those are excluded; when an event ends exactly
  at L nothing is excluded.
* **Crossed / locked book** after every depth event (best bid >= / == best ask).
* **ts_local** never decreases; rows clamped by the normalizer (FLAG_TS_CLAMPED) are counted.
* **Latency:** ts_local - ts_exch for depth events and trades (includes the PC's clock offset).
* **Trades outside the BBO** of the book as of the last depth event before them, by exchange time
  (informational: depth events are 100 ms aggregates, so a fast move can put a trade outside).
  Receive time would mislead: the trade stream can arrive a second or more behind the depth stream.
* **Coverage:** minutes of the UTC day with a synced book receiving updates, and minutes with trades.
* **Grid:** prices and quantities off the date's tick / lot size (FLAG_OFF_GRID).
* **Gaps:** GAP rows, segments and missed update IDs; trade ID gaps.

Verdict: ``pass`` = all integrity checks clean, a complete day, >= 99% coverage and at least one snapshot
verified; ``partial`` = integrity clean but not all of those; ``fail`` = an integrity check failed (a snapshot
mismatch, a crossed book, an off-grid value or ts_local going backwards).

Usage::

    uv run python -m research.data.qc --root data/normalized --symbols BTCUSDT ETHUSDT
    uv run python -m research.data.qc --symbols BTCUSDT --dates 2026-11-05
"""

from __future__ import annotations

import argparse
import html
import json
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow.parquet as pq

from research.data.normalize import day_start_ns
from research.data.refbook import RefBook
from research.data.schema import (
    FLAG_LAST_IN_BATCH, FLAG_OFF_GRID, FLAG_TS_CLAMPED, KIND_DELTA, KIND_GAP, KIND_SNAP_BEGIN, KIND_SNAP_END,
    SIDE_BID, partition_dir,
)

QC_VERSION = 1
TOP_N = 20
COVERAGE_TARGET = 0.99
_COLS = ["ts_local", "ts_exch", "seq", "seq_first", "seg", "kind", "side", "px", "qty", "flags"]


@dataclass
class Snapshot:
    seq: int
    ts: int
    seg: int
    bids: list[tuple[int, int]] = field(default_factory=list)  # best first
    asks: list[tuple[int, int]] = field(default_factory=list)


def _columns(path: Path) -> dict[str, np.ndarray]:
    table = pq.read_table(path, columns=_COLS)
    return {name: table.column(name).to_numpy() for name in _COLS}


def load_snapshots(path: Path, top_n: int = TOP_N) -> list[Snapshot]:
    """REST snapshots of the partition (top_n levels per side), sorted by lastUpdateId."""
    c = _columns(path)
    snaps: list[Snapshot] = []
    begins = np.flatnonzero(c["kind"] == KIND_SNAP_BEGIN)
    ends = np.flatnonzero(c["kind"] == KIND_SNAP_END)
    for b, e in zip(begins, ends):
        side = c["side"][b + 1:e]
        px = c["px"][b + 1:e]
        qty = c["qty"][b + 1:e]
        bid = side == SIDE_BID
        snaps.append(Snapshot(int(c["seq"][b]), int(c["ts_local"][b]), int(c["seg"][b]),
                              list(zip(px[bid][:top_n].tolist(), qty[bid][:top_n].tolist())),
                              list(zip(px[~bid][:top_n].tolist(), qty[~bid][:top_n].tolist()))))
    snaps.sort(key=lambda s: s.seq)
    return snaps


def _window(book: dict[int, int], snap_levels: list[tuple[int, int]], is_bid: bool,
            excluded: set[int]) -> tuple[dict[int, int], dict[int, int]]:
    """(book levels, snapshot levels) within the snapshot's top-N price window, minus excluded prices."""
    want = {p: q for p, q in snap_levels if p not in excluded}
    if not snap_levels:
        return {}, want
    edge = snap_levels[-1][0]
    if is_bid:
        got = {p: q for p, q in book.items() if p >= edge and p not in excluded}
    else:
        got = {p: q for p, q in book.items() if p <= edge and p not in excluded}
    return got, want


def _percentiles_ms(values: np.ndarray) -> dict[str, float] | None:
    if values.size == 0:
        return None
    ms = values / 1e6
    q = np.percentile(ms, [50, 90, 99, 99.9])
    return {"p50": round(float(q[0]), 3), "p90": round(float(q[1]), 3), "p99": round(float(q[2]), 3),
            "p99_9": round(float(q[3]), 3), "max": round(float(ms.max()), 3), "min": round(float(ms.min()), 3)}


def _book_items(path: Path, batch_size: int = 500_000):
    """Streams a book file: yields (rows, column batch) where rows are the rows of one depth event (contiguous,
    the last one flagged) or a single non-delta row; batches are yielded once each (None otherwise) for the
    vectorized checks."""
    pf = pq.ParquetFile(path)
    carry: list[tuple] = []
    for batch in pf.iter_batches(batch_size=batch_size, columns=_COLS):
        arrays = {name: batch.column(name).to_numpy() for name in _COLS}
        yield None, arrays
        for r in zip(*(arrays[name].tolist() for name in _COLS)):
            if r[5] == KIND_DELTA:
                carry.append(r)
                if r[9] & FLAG_LAST_IN_BATCH:
                    yield carry, None
                    carry = []
            else:
                yield [r], None
    if carry:  # a truncated event (shouldn't happen): still applied
        yield carry, None


def run_qc(part: Path, top_n: int = TOP_N) -> dict[str, Any]:
    meta = json.loads((part / "meta.json").read_text(encoding="utf-8"))
    date = meta["date"]
    day0 = day_start_ns(date)
    trade_c = _columns(part / "trade.parquet")
    snaps = load_snapshots(part / "snapshot.parquet", top_n)

    book = RefBook()
    crossed = locked = batches_checked = 0
    first_crossed: dict[str, Any] | None = None
    compared = matched = skipped = 0
    mismatches: list[dict[str, Any]] = []
    book_minutes = np.zeros(1440, dtype=bool)
    trade_minutes = np.zeros(1440, dtype=bool)
    outside_bbo = trades_checked = 0
    outside_ticks = {"1-2": 0, "3-10": 0, ">10": 0}
    si = 0  # next snapshot to compare

    # Vectorized per-batch accumulators
    lat_depth_parts: list[np.ndarray] = []
    nonmono = 0
    prev_ts: int | None = None
    off_grid = int(((trade_c["flags"] & FLAG_OFF_GRID) != 0).sum())
    clamped = int(((trade_c["flags"] & FLAG_TS_CLAMPED) != 0).sum())
    gap_rows = 0

    # Trades are checked against the book by exchange time: the trade stream can arrive a second or more
    # after the depth stream (network bursts), so receive time would compare against a book that moved on
    order = np.lexsort((trade_c["seq"], trade_c["ts_exch"]))
    t_exch = trade_c["ts_exch"][order].tolist()
    t_px = trade_c["px"][order].tolist()
    ti = 0
    n_trades = len(t_exch)
    minutes = (trade_c["ts_local"] - day0) // 60_000_000_000
    trade_minutes[np.unique(minutes[(minutes >= 0) & (minutes < 1440)])] = True

    for event, arrays in _book_items(part / "book.parquet"):
        if arrays is not None:
            ts_col, fl, kd = arrays["ts_local"], arrays["flags"], arrays["kind"]
            last_delta = (kd == KIND_DELTA) & ((fl & FLAG_LAST_IN_BATCH) != 0)
            lat_depth_parts.append((ts_col - arrays["ts_exch"])[last_delta])
            if ts_col.size:
                nonmono += int((np.diff(ts_col) < 0).sum()) + int(prev_ts is not None and ts_col[0] < prev_ts)
                prev_ts = int(ts_col[-1])
            off_grid += int(((fl & FLAG_OFF_GRID) != 0).sum())
            clamped += int(((fl & FLAG_TS_CLAMPED) != 0).sum())
            gap_rows += int((kd == KIND_GAP).sum())
            continue
        row = event[0]
        ts = row[0]
        if row[5] != KIND_DELTA:
            book.apply(row)
            continue
        u, first = row[2], row[3]

        # Trades executed before this event (exchange time), against the book as it stood
        exch = row[1]
        while ti < n_trades and t_exch[ti] < exch:
            if book.valid:
                bid, ask = book.best_bid(), book.best_ask()
                if bid is not None and ask is not None:
                    trades_checked += 1
                    dist = bid[0] - t_px[ti] if t_px[ti] < bid[0] else t_px[ti] - ask[0]
                    if dist > 0:
                        outside_bbo += 1
                        outside_ticks["1-2" if dist <= 2 else "3-10" if dist <= 10 else ">10"] += 1
            ti += 1

        # Snapshots whose lastUpdateId the stream passes with this event
        while si < len(snaps) and snaps[si].seq < u:
            snap = snaps[si]
            si += 1
            if not book.valid or snap.seg != book.seg or snap.seq < book.last_seq or first > snap.seq + 1:
                skipped += 1
                continue
            excluded_b: set[int] = set()
            excluded_a: set[int] = set()
            if first <= snap.seq:  # the event spans L: its levels' state at L is unknown
                for r in event:
                    (excluded_b if r[6] == SIDE_BID else excluded_a).add(r[7])
            got_b, want_b = _window(book.bids, snap.bids, True, excluded_b)
            got_a, want_a = _window(book.asks, snap.asks, False, excluded_a)
            compared += 1
            if got_b == want_b and got_a == want_a:
                matched += 1
            elif len(mismatches) < 20:
                mismatches.append({
                    "lastUpdateId": snap.seq, "ts_local": snap.ts, "event_U": first, "event_u": u,
                    "bids_missing_or_wrong": sorted(set(want_b.items()) - set(got_b.items()))[:10],
                    "bids_extra": sorted(set(got_b.items()) - set(want_b.items()))[:10],
                    "asks_missing_or_wrong": sorted(set(want_a.items()) - set(got_a.items()))[:10],
                    "asks_extra": sorted(set(got_a.items()) - set(want_a.items()))[:10],
                })

        for r in event:
            book.apply(r)
        if book.valid:
            batches_checked += 1
            bid, ask = book.best_bid(), book.best_ask()
            if bid is not None and ask is not None and bid[0] >= ask[0]:
                crossed += 1
                if bid[0] == ask[0]:
                    locked += 1
                if first_crossed is None:
                    first_crossed = {"ts_local": ts, "seq": u, "best_bid": bid, "best_ask": ask}
            m = (ts - day0) // 60_000_000_000
            if 0 <= m < 1440:
                book_minutes[m] = True

    skipped += len(snaps) - si  # never reached by the stream
    lat_depth = np.concatenate(lat_depth_parts) if lat_depth_parts else np.empty(0, dtype=np.int64)
    lat_trade = trade_c["ts_local"] - trade_c["ts_exch"]
    if trade_c["ts_local"].size > 1:
        nonmono += int((np.diff(trade_c["ts_local"]) < 0).sum())

    # -- verdict ----------------------------------------------------------------------------------------
    book_cov = float(book_minutes.mean())
    checks = {
        "snapshots_match": matched == compared,
        "never_crossed": crossed == 0,
        "on_grid": off_grid == 0,
        "ts_local_monotonic": nonmono == 0,
    }
    integrity_ok = all(checks.values())
    complete = bool(meta["day_complete"])
    if not integrity_ok:
        verdict = "fail"
    elif complete and book_cov >= COVERAGE_TARGET and compared > 0:
        verdict = "pass"  # a day can't pass without at least one verified snapshot
    else:
        verdict = "partial"

    hours = book_minutes.reshape(24, 60).mean(axis=1)
    qc = {
        "qc_version": QC_VERSION,
        "symbol": meta["symbol"],
        "date": date,
        "verdict": verdict,
        "checks": checks,
        "day_complete": complete,
        "coverage": {
            "book_minutes": int(book_minutes.sum()), "book_fraction": round(book_cov, 5),
            "trade_minutes": int(trade_minutes.sum()), "trade_fraction": round(float(trade_minutes.mean()), 5),
            "book_by_hour": [round(float(h), 3) for h in hours],
            "target": COVERAGE_TARGET,
        },
        "snapshots": {"total": len(snaps), "compared": compared, "matched": matched, "skipped": skipped,
                      "top_n": top_n, "mismatches": mismatches},
        "book": {"rows": meta["rows"]["book"], "depth_events_checked": batches_checked, "crossed": crossed,
                 "locked": locked, "first_crossed": first_crossed},
        "gaps": {"gap_rows": gap_rows, "segments": meta["segments"],
                 "trade_id_gaps": meta["stats"]["trade_id_gaps"], "trades_missing": meta["stats"]["trades_missing"]},
        "trades": {"rows": meta["rows"]["trade"], "checked_against_bbo": trades_checked,
                   "outside_bbo": outside_bbo, "outside_bbo_by_ticks": outside_ticks,
                   "outside_bbo_fraction": round(outside_bbo / trades_checked, 5) if trades_checked else None},
        "timestamps": {"non_monotonic": nonmono,
                       "clamped_rows": clamped,
                       "latency_ms_depth": _percentiles_ms(lat_depth),
                       "latency_ms_trade": _percentiles_ms(lat_trade)},
        "grid": {"off_grid_rows": off_grid, "spec": meta["spec"]},
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    _write(part / "qc.json", json.dumps(qc, indent=2))
    _write(part / "qc.html", render_html(qc))
    return qc


def _write(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".part")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def render_html(qc: dict[str, Any]) -> str:
    """A self-contained one-page summary."""
    color = {"pass": "#1a7f37", "partial": "#9a6700", "fail": "#cf222e"}[qc["verdict"]]
    e = html.escape

    def row(label: str, value: Any, ok: bool | None = None) -> str:
        mark = "" if ok is None else (" ✓" if ok else " ✗")
        cls = "" if ok is None else (' class="ok"' if ok else ' class="bad"')
        return f"<tr><th>{e(label)}</th><td{cls}>{e(str(value))}{mark}</td></tr>"

    c, s, b, t, ts = qc["coverage"], qc["snapshots"], qc["book"], qc["trades"], qc["timestamps"]
    bars = "".join(
        f'<div class="bar" title="{h:02d}:00 UTC: {v:.0%}"><div style="height:{v * 100:.0f}%"></div>'
        f"<span>{h:02d}</span></div>" for h, v in enumerate(c["book_by_hour"]))
    lat_d = ts["latency_ms_depth"] or {}
    lat_t = ts["latency_ms_trade"] or {}
    rows = "".join([
        row("Snapshots matched (top %d)" % s["top_n"], f"{s['matched']} / {s['compared']} compared, "
            f"{s['skipped']} not comparable", qc["checks"]["snapshots_match"]),
        row("Crossed / locked after an event", f"{b['crossed']} / {b['locked']} of {b['depth_events_checked']:,}",
            qc["checks"]["never_crossed"]),
        row("Off-grid rows", qc["grid"]["off_grid_rows"], qc["checks"]["on_grid"]),
        row("ts_local decreasing", ts["non_monotonic"], qc["checks"]["ts_local_monotonic"]),
        row("Book coverage", f"{c['book_minutes']} / 1440 min ({c['book_fraction']:.2%})",
            c["book_fraction"] >= c["target"]),
        row("Trade coverage", f"{c['trade_minutes']} / 1440 min ({c['trade_fraction']:.2%})"),
        row("Day complete", qc["day_complete"], qc["day_complete"]),
        row("Gaps (GAP rows)", qc["gaps"]["gap_rows"]),
        row("Trade ID gaps / trades missing", f"{qc['gaps']['trade_id_gaps']} / {qc['gaps']['trades_missing']:,}"),
        row("Book rows / trades", f"{b['rows']:,} / {t['rows']:,}"),
        row("Trades outside BBO by exchange time (informational)", f"{t['outside_bbo']:,} of "
            f"{t['checked_against_bbo']:,} ({(t['outside_bbo_fraction'] or 0):.2%}); by ticks "
            + ", ".join(f"{k}: {v:,}" for k, v in t["outside_bbo_by_ticks"].items())),
        row("Clamped ts_local rows", f"{ts['clamped_rows']:,}"),
        row("Latency depth ms (p50 / p99 / max)", f"{lat_d.get('p50')} / {lat_d.get('p99')} / {lat_d.get('max')}"),
        row("Latency trade ms (p50 / p99 / max)", f"{lat_t.get('p50')} / {lat_t.get('p99')} / {lat_t.get('max')}"),
        row("Tick / lot", f"{qc['grid']['spec']['tick_size_text']} / {qc['grid']['spec']['lot_size_text']}"),
    ])
    mism = ""
    if s["mismatches"]:
        mism = "<h2>First snapshot mismatches</h2><pre>" + e(json.dumps(s["mismatches"][:5], indent=1)) + "</pre>"
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>QC {e(qc['symbol'])} {e(qc['date'])}</title>
<style>
:root {{ --fg:#1f2328; --bg:#ffffff; --muted:#59636e; --line:#d1d9e0; --bar:#0969da; }}
@media (prefers-color-scheme: dark) {{ :root {{ --fg:#e6edf3; --bg:#0d1117; --muted:#9198a1; --line:#3d444d; --bar:#4493f8; }} }}
body {{ font: 14px/1.45 system-ui, sans-serif; color: var(--fg); background: var(--bg); margin: 24px auto; max-width: 860px; padding: 0 16px; }}
h1 {{ font-size: 20px; margin: 0 0 4px; }} h2 {{ font-size: 16px; margin-top: 28px; }}
.verdict {{ display:inline-block; padding: 2px 10px; border-radius: 12px; color: #fff; background: {color}; font-weight: 600; }}
table {{ border-collapse: collapse; width: 100%; margin-top: 16px; }}
th, td {{ text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); vertical-align: top; }}
th {{ font-weight: 500; color: var(--muted); width: 42%; }}
td.ok {{ color: #1a7f37; }} td.bad {{ color: #cf222e; font-weight: 600; }}
.bars {{ display: flex; gap: 2px; height: 90px; align-items: flex-end; margin-top: 8px; }}
.bar {{ flex: 1; height: 100%; display: flex; flex-direction: column; justify-content: flex-end; position: relative; }}
.bar div {{ background: var(--bar); border-radius: 2px 2px 0 0; }}
.bar span {{ font-size: 10px; color: var(--muted); text-align: center; }}
pre {{ overflow-x: auto; font-size: 12px; }}
</style></head><body>
<h1>{e(qc['symbol'])} · {e(qc['date'])} (UTC)</h1>
<span class="verdict">{e(qc['verdict'].upper())}</span>
<table>{rows}</table>
<h2>Book coverage by hour (UTC)</h2>
<div class="bars">{bars}</div>
{mism}
<p style="color:var(--muted);font-size:12px;margin-top:24px">qc_version {qc['qc_version']} · {e(qc['created_utc'])}</p>
</body></html>
"""


def partitions(root: Path | str, symbols: list[str], dates: list[str] | None) -> list[Path]:
    parts = []
    for symbol in symbols:
        base = partition_dir(root, symbol, "x").parent
        if not base.exists():
            continue
        for d in sorted(base.glob("date=*")):
            date = d.name.split("=", 1)[1]
            if (dates is None or date in dates) and (d / "meta.json").exists():
                parts.append(d)
    return parts


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="QC for normalized partitions (writes qc.json and qc.html).")
    ap.add_argument("--root", default="data/normalized")
    ap.add_argument("--symbols", nargs="+", required=True)
    ap.add_argument("--dates", nargs="+")
    ap.add_argument("--top-n", type=int, default=TOP_N)
    args = ap.parse_args(argv)
    worst = 0
    for part in partitions(args.root, args.symbols, args.dates):
        t0 = time.perf_counter()
        qc = run_qc(part, args.top_n)
        s, c = qc["snapshots"], qc["coverage"]
        print(f"{qc['symbol']} {qc['date']}  {qc['verdict'].upper():8s} snapshots {s['matched']}/{s['compared']}  "
              f"crossed {qc['book']['crossed']}  coverage {c['book_fraction']:.2%}  "
              f"off-grid {qc['grid']['off_grid_rows']}  ({time.perf_counter() - t0:.0f} s)  -> {part / 'qc.html'}")
        worst = max(worst, {"pass": 0, "partial": 1, "fail": 2}[qc["verdict"]])
    return 2 if worst == 2 else 0


if __name__ == "__main__":
    raise SystemExit(main())
