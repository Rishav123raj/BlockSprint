"""A paper session whose "paper" side is a simulator run (tests of the parity report and calibration)."""

import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

import polars as pl

from research.live.paper_session import replay_session

FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
START = 1_793_844_000_000_000_000  # 2026-11-05 02:00:00 UTC, the fixture's first second


def _iso(ns: int) -> str:
    return datetime.fromtimestamp(ns // 10**9, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + f".{ns % 10**9:09d}Z"


def make_session(root: Path, queue_model: str = "cancel_from_back", seed: int = 1) -> Path:
    """A session on the 1-minute fixture whose paper journal is the replay (at queue_model, seed) of itself."""
    session = root / "s"
    (session / "packed" / "session").mkdir(parents=True)
    shutil.copy(FIXTURE, session / "packed" / "session" / "BTCUSDT.bse")
    (session / "packed" / "session" / "manifest.json").write_text(json.dumps(
        {"files": [{"symbol": "BTCUSDT", "file": "BTCUSDT.bse"}]}))
    strategy = {"python": "research.strategies.passive_quoter:PassiveQuoter",
                "params": {"size": "0.001", "refresh_ms": 100, "max_position": "0.005"}}
    (session / "protocol.json").write_text(json.dumps({"symbol": "BTCUSDT", "strategy": strategy,
                                                       "callback_mode": "timer_only", "queue_model": queue_model,
                                                       "owner": "S1"}))
    (session / "paper").mkdir()
    window = {"start_ns": START + 5 * 10**9, "trading_ns": START + 5 * 10**9, "end_ns": START + 50 * 10**9}
    (session / "paper" / "session_meta.json").write_text(json.dumps(
        {**window, "measured_latency": {"md_delay_ms": {"mean": 1.0, "p10": 0.5, "p90": 1.5},
                                        "order_rtt_ms": {"p50": 2.0}}}))
    replay_session(session, [seed])

    # The "paper" journal: the replay's own requests and fills, as the live runner would have written them
    run = session / "sim" / f"seed_{seed}"
    # ns as integers before iterating (iter_rows gives datetimes with microseconds)
    orders = pl.read_parquet(run / "orders.parquet").filter(pl.col("request").is_in(["new", "replace"])) \
        .with_columns(pl.col("sent_ns").cast(pl.Int64))
    fills = pl.read_parquet(run / "exec_reports.parquet").filter(pl.col("type") == "fill") \
        .with_columns(pl.col("exchange_ns").cast(pl.Int64))
    lines = [{"t": o["sent_ns"],
              "kind": "request", "action": "submit" if o["request"] == "new" else "replace", "order_id": o["order_id"],
              "quantity": f"{o['quantity']:.8f}"} for o in orders.iter_rows(named=True)]
    for f in fills.iter_rows(named=True):
        ns = f["exchange_ns"]
        lines.append({"t": ns, "kind": "report", "type": "fill", "timestamp": _iso(ns), "order_id": f["order_id"],
                      "side": f["side"], "last_px": f"{f['last_px']:.8f}", "last_qty": f"{f['last_qty']:.8f}",
                      "fee": f"{f['fee']:.8f}", "liquidity": f["liquidity"]})
    (session / "paper" / "session.jsonl").write_text("\n".join(json.dumps(x) for x in lines))
    return session
