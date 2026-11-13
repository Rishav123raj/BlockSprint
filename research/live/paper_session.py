"""The paper session protocol (P7.4, docs/parity_protocol.md): one strategy, one window, run twice.

``run`` records the market and paper-trades the strategy in the same window:

1. the recorder (research.data.recorder) starts writing the symbol's raw streams into the session directory;
2. after ``--lead-s`` (so the recording has a snapshot and a live book before the session starts) the server
   starts with the session's queue model, then the v2 feed bridge, and the session waits for the feed to sync;
3. the live runner paper-trades the strategy for ``--minutes``, journaling into ``paper/``;
4. the bridge and the server stop; the recorder keeps going for ``--tail-s`` (markouts look past the end).

``replay`` turns the recording into a dataset (normalize, QC, pack with ``--allow-partial``: a session covers a
few minutes of a day) and runs the same strategy class with the same parameters in the simulator over exactly
the paper window: the trading start to the end, the same queue model, gap policy ``continue`` (paper mode has
no halt), the paper session's measured delays as the latency, once per seed. Results go to ``sim/seed_<n>/``.

Usage::

    uv run python -m research.live.paper_session run --strategy research.strategies.passive_quoter:PassiveQuoter \\
        --params '{"size": "0.001", "refresh_ms": 500}' --symbol BTCUSDT --minutes 15 --session data/paper/q1
    uv run python -m research.live.paper_session replay --session data/paper/q1 --seeds 1 2 3
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import requests

from research.live.feed_bridge import engine_name

log = logging.getLogger("paper_session")
ROOT = Path(__file__).resolve().parents[2]
SERVER_CANDIDATES = [ROOT / "build" / "Release" / "crypto_matching_engine.exe", ROOT / "build" / "crypto_matching_engine"]


def _server_binary() -> Path:
    for p in SERVER_CANDIDATES:
        if p.exists():
            return p
    raise FileNotFoundError("the server isn't built (cmake --build build --config Release)")


def _wait_synced(engine: str, symbol: str, timeout_s: float = 60.0) -> None:
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        try:
            st = requests.get(f"{engine}/api/market-feed", timeout=2).json()["data"]["symbols"].get(symbol, {})
            if st.get("synced"):
                return
        except requests.RequestException:
            pass
        time.sleep(0.5)
    raise TimeoutError(f"the market feed for {symbol} didn't sync within {timeout_s:.0f} s")


def _wait_up(engine: str, timeout_s: float = 20.0) -> None:
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        try:
            requests.get(f"{engine}/api/market-feed", timeout=1)
            return
        except requests.RequestException:
            time.sleep(0.2)
    raise TimeoutError("the server didn't start")


def run_session(strategy_path: str, params: dict[str, Any], symbol: str, minutes: float, session: Path,
                queue_model: str = "prob_power:2", port: int = 8095, lead_s: float = 20.0, tail_s: float = 60.0,
                owner: str = "S1") -> dict[str, Any]:
    from research.experiments.schema import make_python_strategy
    from research.live.runner import LiveRunner

    session.mkdir(parents=True, exist_ok=True)
    if (session / "protocol.json").exists():
        raise FileExistsError(f"{session} already holds a session")
    engine = f"http://127.0.0.1:{port}"
    name = engine_name(symbol)
    strategy = make_python_strategy({"python": strategy_path, "params": params})  # fails before anything starts
    protocol = {"format": "bs_paper.protocol/1", "strategy": {"python": strategy_path, "params": params},
                "callback_mode": getattr(strategy, "callback_mode", "every_event"), "symbol": symbol,
                "engine_symbol": name, "minutes": minutes, "queue_model": queue_model, "owner": owner,
                "lead_s": lead_s, "tail_s": tail_s, "created_utc": datetime.now(timezone.utc).isoformat()}
    (session / "protocol.json").write_text(json.dumps(protocol, indent=2), encoding="utf-8")

    py = [sys.executable, "-u", "-m"]
    record_s = lead_s + minutes * 60 + tail_s + 60  # +60: the feed's sync and stop
    procs: list[subprocess.Popen] = []
    logs = []
    try:
        f = open(session / "recorder.out", "w")
        logs.append(f)
        procs.append(subprocess.Popen(py + ["research.data.recorder", "--symbols", symbol, "--out", str(session / "raw"),
                                            "--duration", str(record_s), "--log-file", str(session / "recorder.log")],
                                      cwd=ROOT, stdout=f, stderr=subprocess.STDOUT))
        recorder = procs[-1]
        log.info("recording %s for %.0f s; the session starts in %.0f s", symbol, record_s, lead_s)
        time.sleep(lead_s)
        if recorder.poll() is not None:
            raise RuntimeError(f"the recorder stopped (see {session / 'recorder.out'})")

        procs.append(subprocess.Popen([str(_server_binary()), "--port", str(port), "--no-log", "--feed-queue-model",
                                       queue_model], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        _wait_up(engine)
        f = open(session / "bridge.out", "w")
        logs.append(f)
        procs.append(subprocess.Popen(py + ["research.live.feed_bridge", "--symbols", symbol, "--engine", engine],
                                      cwd=ROOT, stdout=f, stderr=subprocess.STDOUT))
        _wait_synced(engine, name)
        log.info("feed synced; paper trading %s on %s for %.1f min", strategy_path, name, minutes)

        runner = LiveRunner(strategy, [name], engine, owner, out_dir=session / "paper")
        meta = asyncio.run(runner.run(minutes * 60))
        feed_stats = requests.get(f"{engine}/api/market-feed", timeout=3).json()["data"]
        (session / "paper" / "feed_stats.json").write_text(json.dumps(feed_stats, indent=2), encoding="utf-8")
    finally:
        for p in procs[1:]:  # bridge, server: now; the recorder runs its tail
            p.terminate()
        for p in procs[1:]:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
    log.info("session over: %s; recording the tail", json.dumps(meta["stats"]))
    recorder.wait(timeout=record_s + 120)
    for f in logs:
        f.close()
    return meta


def _dates(start_ns: int, end_ns: int) -> list[str]:
    d0 = datetime.fromtimestamp(start_ns / 1e9, timezone.utc).date()
    d1 = datetime.fromtimestamp(end_ns / 1e9, timezone.utc).date()
    out = []
    while d0 <= d1:
        out.append(d0.isoformat())
        d0 = d0.fromordinal(d0.toordinal() + 1)
    return out


def default_latency(meta: dict[str, Any]) -> dict[str, Any]:
    """The sim's latency for a paper session, from what the runner measured: the depth delay as a normal with its
    mean and spread (the server batches depth every 100 ms), each order leg as half the median round trip."""
    md = (meta.get("measured_latency") or {}).get("md_delay_ms") or {"mean": 50.0, "p10": 10.0, "p90": 90.0}
    rtt = (meta.get("measured_latency") or {}).get("order_rtt_ms") or {"p50": 2.0}
    sd_ms = max(0.1, (md["p90"] - md["p10"]) / 2.563)  # p10..p90 of a normal is 2.563 sd
    leg_us = max(1.0, rtt["p50"] * 1000 / 2)
    return {"feed": {"model": "normal", "mean_us": round(md["mean"] * 1000), "sd_us": round(sd_ms * 1000), "min_us": 0},
            "entry": {"model": "constant", "us": round(leg_us)}, "ack": {"model": "constant", "us": round(leg_us)}}


def build_dataset(session: Path) -> Path:
    from research.data import normalize, pack, qc

    protocol = json.loads((session / "protocol.json").read_text(encoding="utf-8"))
    meta = json.loads((session / "paper" / "session_meta.json").read_text(encoding="utf-8"))
    sym = protocol["symbol"]
    dates = _dates(meta["start_ns"], meta["end_ns"])
    packed = session / "packed" / "session"
    if (packed / "manifest.json").exists():
        return packed
    norm = session / "normalized"
    if normalize.main(["--raw", str(session / "raw"), "--out", str(norm), "--symbols", sym, "--dates", *dates]) != 0:
        raise RuntimeError("normalizing the session failed")
    if qc.main(["--root", str(norm), "--symbols", sym, "--dates", *dates]) == 2:
        raise RuntimeError(f"QC failed for the session's data (see {norm})")
    pack.main(["pack", "--norm", str(norm), "--out", str(session / "packed"), "--symbols", sym, "--from", dates[0],
               "--to", dates[-1], "--name", "session", "--allow-partial"])
    return packed


def replay_session(session: Path, seeds: list[int], latency: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    from research.experiments.schema import make_python_strategy
    from research.sim.results import Results

    protocol = json.loads((session / "protocol.json").read_text(encoding="utf-8"))
    meta = json.loads((session / "paper" / "session_meta.json").read_text(encoding="utf-8"))
    dataset = build_dataset(session)
    latency = latency or default_latency(meta)
    out = []
    for seed in seeds:
        cfg = {"dataset": str(dataset), "symbol": protocol["symbol"],
               "window": {"from": meta["trading_ns"], "to": meta["end_ns"]},
               "latency": latency, "queue_model": protocol["queue_model"], "seed": seed, "gap_policy": "continue",
               "strategy": {"name": "null", "owner": protocol["owner"]},
               "output": {"dir": str(session / "sim" / f"seed_{seed}"), "sample_ms": 1000}}
        strategy = make_python_strategy(protocol["strategy"])
        res = Results.run(cfg, strategy, protocol["callback_mode"])
        target = session / "sim" / f"seed_{seed}"
        for table, frame in res.to_polars(fixed="float").items():
            frame.write_parquet(target / f"{table}.parquet")
        (target / "run_meta.json").write_text(json.dumps(res.meta, indent=2, default=str), encoding="utf-8")
        acct = res.meta["sessions"][0]["exchange_account"]
        out.append({"seed": seed, "fills": acct["fills"], "base_volume": acct["base_volume"], "pnl": acct["pnl"]})
        log.info("sim seed %d: %s", seed, out[-1])
    (session / "sim" / "replay.json").write_text(json.dumps({"latency": latency, "runs": out}, indent=2),
                                                  encoding="utf-8")
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="record the market and paper-trade a strategy in the same window")
    r.add_argument("--strategy", required=True, help="package.module:Class")
    r.add_argument("--params", default="{}")
    r.add_argument("--symbol", required=True, help="the Binance symbol, e.g. BTCUSDT")
    r.add_argument("--minutes", type=float, required=True)
    r.add_argument("--session", required=True, help="a new directory for the session")
    r.add_argument("--queue-model", default="prob_power:2")
    r.add_argument("--port", type=int, default=8095)
    r.add_argument("--lead-s", type=float, default=20.0)
    r.add_argument("--tail-s", type=float, default=60.0)
    rp = sub.add_parser("replay", help="the session's window in the simulator")
    rp.add_argument("--session", required=True)
    rp.add_argument("--seeds", type=int, nargs="+", default=[1])
    rp.add_argument("--latency", help="the sim's latency (JSON); default: from the session's measured delays")
    args = p.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    if args.cmd == "run":
        meta = run_session(args.strategy, json.loads(args.params), args.symbol.upper(), args.minutes,
                           Path(args.session), args.queue_model, args.port, args.lead_s, args.tail_s)
        print(json.dumps({"stats": meta["stats"], "accounts": meta["accounts"],
                          "measured_latency": meta["measured_latency"]}, indent=2))
    else:
        runs = replay_session(Path(args.session), args.seeds, json.loads(args.latency) if args.latency else None)
        print(json.dumps(runs, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
