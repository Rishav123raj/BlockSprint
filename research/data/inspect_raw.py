"""Quality report for a recorded raw archive (see :mod:`research.data.recorder`).

Checks, per symbol and per WebSocket connection:

* diff-depth continuity: every event's first update ID ``U`` equals the previous event's ``u + 1``
* trade continuity: trade IDs increase by exactly one
* snapshot sync: for each REST snapshot, a later depth event on a live connection satisfies
  Binance's rule ``U <= lastUpdateId + 1 <= u``, i.e. the local book can be built from it
* handover: when a connection replaces another within one recorder run (24 h rollover or
  reconnect), its first depth event must start at or before the update after the previous
  connection's last one, so no update falls between the two
* restarts: each recorder run numbers its connections from 1; a run is detected by its first
  ``connecting`` event. Updates missed while no recorder was running are reported as restart
  gaps, which are expected (normalization starts a new segment there) and not counted as issues
* latency: local receive time minus exchange event time ``E`` (includes clock offset)

Usage::

    uv run python -m research.data.inspect_raw --root data/raw --symbols BTCUSDT ETHUSDT
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from research.data.rawio import archive_files, read_raw


@dataclass
class ConnectionStats:
    depth_events: int = 0
    depth_gaps: int = 0
    trades: int = 0
    trade_gaps: int = 0
    last_u: int | None = None
    last_trade_id: int | None = None
    first_t: int | None = None
    last_t: int | None = None
    depth_ranges: list[tuple[int, int]] = field(default_factory=list)  # (U, u) per event


@dataclass
class SymbolReport:
    symbol: str
    files: int = 0
    envelopes: int = 0
    kinds: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    connections: dict[tuple[int, int], ConnectionStats] = field(default_factory=dict)  # (run, conn)
    snapshots: list[int] = field(default_factory=list)  # lastUpdateId per snapshot
    events: list[tuple[int, int | None, str, str]] = field(default_factory=list)  # (t, conn, name, reason)
    latencies_ms: list[float] = field(default_factory=list)

    run: int = 0
    _run_conns: set[int] = field(default_factory=set)

    def _connection_changes(self) -> list[tuple[tuple[int, int], tuple[int, int], int]]:
        """((run, conn) before, (run, conn) after, missed updates) for consecutive connections."""
        changes = []
        conns = sorted((key, c) for key, c in self.connections.items() if c.depth_ranges)
        for (prev_key, prev), (next_key, nxt) in zip(conns, conns[1:]):
            changes.append((prev_key, next_key, max(0, nxt.depth_ranges[0][0] - prev.last_u - 1)))
        return changes

    def handover_gaps(self) -> list[tuple[int, int, int]]:
        """(previous conn, next conn, missed updates) for lossy connection changes within one run."""
        return [(a[1], b[1], n) for a, b, n in self._connection_changes() if a[0] == b[0] and n > 0]

    def restart_gaps(self) -> list[tuple[int, int, int]]:
        """(previous run, next run, missed updates) while no recorder was running."""
        return [(a[0], b[0], n) for a, b, n in self._connection_changes() if a[0] != b[0]]

    def snapshot_syncable(self, last_update_id: int) -> bool:
        target = last_update_id + 1
        return any(U <= target <= u for c in self.connections.values() for U, u in c.depth_ranges)


def inspect_symbol(root: Path, symbol: str) -> SymbolReport:
    report = SymbolReport(symbol)
    paths = archive_files(root, symbol)
    report.files = len(paths)
    for path in paths:
        for env in read_raw(path):
            report.envelopes += 1
            kind = env["k"]
            report.kinds[kind] += 1
            if kind == "ws":
                _ws(report, env)
            elif kind == "snapshot":
                report.snapshots.append(json.loads(env["d"])["lastUpdateId"])
            elif kind == "event":
                if env["d"]["event"] == "connecting":
                    if env.get("c") in report._run_conns:  # connection IDs restart: a new recorder run
                        report.run += 1
                        report._run_conns = set()
                    report._run_conns.add(env.get("c"))
                report.events.append((env["t"], env.get("c"), env["d"]["event"], str(env["d"].get("reason", ""))))
    return report


def _ws(report: SymbolReport, env: dict) -> None:
    conn = report.connections.setdefault((report.run, env["c"]), ConnectionStats())
    t = env["t"]
    conn.first_t = t if conn.first_t is None else conn.first_t
    conn.last_t = t
    data = json.loads(env["d"])["data"]
    report.latencies_ms.append((t - data["E"] * 1_000_000) / 1e6)
    if data["e"] == "depthUpdate":
        conn.depth_events += 1
        if conn.last_u is not None and data["U"] != conn.last_u + 1:
            conn.depth_gaps += 1
        conn.last_u = data["u"]
        conn.depth_ranges.append((data["U"], data["u"]))
    elif data["e"] == "trade":
        conn.trades += 1
        if conn.last_trade_id is not None and data["t"] != conn.last_trade_id + 1:
            conn.trade_gaps += 1
        conn.last_trade_id = data["t"]


def _percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def _ts(ns: int | None) -> str:
    if ns is None:
        return "-"
    return datetime.fromtimestamp(ns / 1e9, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + "Z"


def format_report(r: SymbolReport) -> str:
    lines = [f"=== {r.symbol}: {r.files} file(s), {r.envelopes} envelopes, kinds {dict(r.kinds)}"]
    for (run, cid), c in sorted(r.connections.items()):
        span = (c.last_t - c.first_t) / 1e9 if c.first_t is not None else 0.0
        lines.append(
            f"  run {run} connection {cid}: {_ts(c.first_t)} .. {_ts(c.last_t)} ({span:.1f}s) | "
            f"depth events {c.depth_events}, sequence gaps {c.depth_gaps} | "
            f"trades {c.trades}, trade-id gaps {c.trade_gaps}"
        )
    changes = r._connection_changes()
    handovers = [c for c in changes if c[0][0] == c[1][0]]
    if handovers:
        gaps = r.handover_gaps()
        lines.append("  handover: " + (f"seamless ({len(handovers)} connection change(s) within runs)" if not gaps
                                       else "; ".join(f"conn {a}->{b} missed {n} updates" for a, b, n in gaps)))
    for a, b, n in r.restart_gaps():
        lines.append(f"  restart gap (expected, recorder not running): run {a} -> run {b} missed {n} updates")
    for last_update_id in r.snapshots:
        verdict = "syncable" if r.snapshot_syncable(last_update_id) else "NOT syncable (no covering depth event)"
        lines.append(f"  snapshot lastUpdateId={last_update_id}: {verdict}")
    if r.latencies_ms:
        lines.append(
            f"  receive - exchange time (ms): p50 {_percentile(r.latencies_ms, 0.5):.1f}, "
            f"p99 {_percentile(r.latencies_ms, 0.99):.1f}, max {max(r.latencies_ms):.1f}"
        )
    for t, conn, name, reason in r.events:
        lines.append(f"  event {_ts(t)} conn={conn} {name} {reason}".rstrip())
    return "\n".join(lines)


def report_ok(r: SymbolReport) -> bool:
    gaps = sum(c.depth_gaps + c.trade_gaps for c in r.connections.values())
    return (gaps == 0 and not r.handover_gaps() and bool(r.snapshots)
            and all(r.snapshot_syncable(s) for s in r.snapshots))


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Quality report for a raw market-data archive.")
    p.add_argument("--root", type=Path, default=Path("data/raw"))
    p.add_argument("--symbols", nargs="+", required=True)
    args = p.parse_args(argv)
    ok = True
    for symbol in args.symbols:
        r = inspect_symbol(args.root, symbol.upper())
        print(format_report(r))
        ok &= report_ok(r)
    print("RESULT:", "OK" if ok else "ISSUES FOUND")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
