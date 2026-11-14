"""Soak test for connector v2 (P7.8): run the server and the feed bridge for hours, and check them against Binance.

The v1 connector is removed once v2 passes this. It starts a fresh server and ``research.live.feed_bridge`` for the
symbols and, until ``--hours`` have passed:

* every ``--poll-s`` (5 s): both processes are alive, and which symbols are synced (``/api/market-feed``), to time
  every resync (from losing sync to having it again);
* every ``--check-s`` (60 s), per synced symbol: the server's top 10 levels a side (``/api/depth``) against
  Binance's REST depth, read just before and just after it (the better of the two counts: the reads are some
  hundreds of ms apart and the book moves), whether the server's book is crossed, and both processes' memory.

Each check is a line in ``<out>/soak.jsonl``; ``<out>/soak.json`` holds the summary and the verdict, rewritten after
every check, so a run that is stopped early still has one. The bridge logs into ``<out>/bridge.out``.

The verdict passes when, over the whole run:

* both processes stayed up;
* every resync took at most ``MAX_RESYNC_S``;
* per symbol, the median share of Binance's 20 top levels the server has at the same price and quantity is at least
  ``MIN_MEDIAN_MATCH``, and the share at the same price never stays below ``STUCK_MATCH`` for ``STUCK_CHECKS``
  checks in a row;
* the server's book was never crossed while synced;
* each process's memory grew by at most ``MAX_GROWTH_MB_PER_H`` over the second half of the run (a fit), so it
  levels off;
* the run lasted at least 24 h and saw at least one reconnect to Binance (Binance closes every connection after
  24 h), every one recovered; a shorter run can pass everything else but is reported as not covering it.

Usage::

    uv run python -m research.live.soak --hours 26 --out data/soak/2026-11-14
"""

from __future__ import annotations

import argparse
import json
import logging
import subprocess
import sys
import time
from decimal import Decimal
from pathlib import Path
from typing import Any

import numpy as np
import requests

from research.live.feed_bridge import engine_name
from research.live.paper_session import ROOT, _server_binary, _wait_up

log = logging.getLogger("soak")
BINANCE_REST = "https://api.binance.com"
SYMBOLS = ["BTCUSDT", "ETHUSDT", "BNBUSDT", "SOLUSDT", "XRPUSDT"]
MAX_RESYNC_S = 30.0
MIN_MEDIAN_MATCH = 0.9
STUCK_MATCH, STUCK_CHECKS = 0.5, 5
MAX_GROWTH_MB_PER_H = 20.0
RECONNECT_HOURS = 24.0


# ---- measures (pure) ----

def _levels(side: list[list[str]]) -> dict[Decimal, Decimal]:
    return {Decimal(p).normalize(): Decimal(q).normalize() for p, q in side}


def level_match(binance: dict[str, Any], server: dict[str, Any]) -> dict[str, float]:
    """The share of Binance's levels (both sides) the server has at the same price and quantity (``qty``) and at the
    same price (``price``)."""
    n = same_qty = same_px = 0
    for side in ("bids", "asks"):
        b, s = _levels(binance.get(side, [])), _levels(server.get(side, []))
        n += len(b)
        same_px += sum(1 for p in b if p in s)
        same_qty += sum(1 for p, q in b.items() if s.get(p) == q)
    return {"qty": same_qty / n if n else 0.0, "price": same_px / n if n else 0.0}


def crossed(server: dict[str, Any]) -> bool:
    bids, asks = server.get("bids") or [], server.get("asks") or []
    return bool(bids and asks and Decimal(bids[0][0]) >= Decimal(asks[0][0]))


def growth_mb_per_h(t_s: list[float], mb: list[float]) -> float | None:
    """The memory's slope over the second half of the run (MB per hour), or None with too few points."""
    t, m = np.asarray(t_s, dtype=float), np.asarray(mb, dtype=float)
    if len(t) < 4 or t[-1] - t[0] <= 0:
        return None
    half = t >= t[0] + (t[-1] - t[0]) / 2
    if half.sum() < 2:
        return None
    return float(np.polyfit(t[half] / 3600, m[half], 1)[0])


def verdict(checks: list[dict[str, Any]], resyncs: list[dict[str, Any]], reconnects: int, hours: float,
            died: str | None) -> dict[str, Any]:
    out: dict[str, Any] = {"hours": round(hours, 3), "checks": len(checks), "reconnects": reconnects}
    crit: dict[str, bool] = {"processes_up": died is None}
    if died:
        out["died"] = died
    slow = [r for r in resyncs if r["seconds"] is None or r["seconds"] > MAX_RESYNC_S]
    out["resyncs"] = len(resyncs)
    out["slowest_resync_s"] = max((r["seconds"] for r in resyncs if r["seconds"] is not None), default=None)
    crit["resyncs_fast"] = not slow
    per: dict[str, Any] = {}
    for sym in sorted({s for c in checks for s in c["symbols"]}):
        rows = [c["symbols"][sym] for c in checks if sym in c["symbols"] and c["symbols"][sym].get("match")]
        qty = [r["match"]["qty"] for r in rows]
        run = worst = 0
        for r in rows:
            run = run + 1 if r["match"]["price"] < STUCK_MATCH else 0
            worst = max(worst, run)
        per[sym] = {"checks": len(rows), "median_match": float(np.median(qty)) if qty else None,
                    "min_match": min(qty) if qty else None, "longest_low_run": worst,
                    "crossed": sum(1 for c in checks if c["symbols"].get(sym, {}).get("crossed"))}
    out["symbols"] = per
    crit["book_matches"] = bool(per) and all(p["median_match"] is not None and p["median_match"] >= MIN_MEDIAN_MATCH
                                             and p["longest_low_run"] < STUCK_CHECKS for p in per.values())
    crit["never_crossed"] = all(p["crossed"] == 0 for p in per.values())
    t = [c["t_s"] for c in checks]
    growth = {}
    for proc in ("server", "bridge"):
        pts = [(c["t_s"], c["memory_mb"][proc]) for c in checks if c.get("memory_mb", {}).get(proc) is not None]
        growth[proc] = growth_mb_per_h([p[0] for p in pts], [p[1] for p in pts]) if pts else None
        out[f"{proc}_memory_mb"] = {"first": pts[0][1], "last": pts[-1][1]} if pts else None
    out["memory_growth_mb_per_h"] = growth
    crit["memory_levels_off"] = all(g is None or g <= MAX_GROWTH_MB_PER_H for g in growth.values()) and bool(t)
    covered = hours >= RECONNECT_HOURS and reconnects >= 1
    out["covers_24h_reconnect"] = covered
    out["criteria"] = crit
    out["pass"] = all(crit.values()) and covered
    return out


# ---- the run ----

def rss_mb(pid: int) -> float | None:
    """A process's resident memory in MB (Windows: working set; elsewhere /proc)."""
    try:
        if sys.platform == "win32":
            import ctypes
            from ctypes import wintypes

            class Counters(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                            ("PeakPagefileUsage", ctypes.c_size_t)]

            h = ctypes.windll.kernel32.OpenProcess(0x1000 | 0x0010, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ
            if not h:
                return None
            try:
                c = Counters()
                c.cb = ctypes.sizeof(c)
                if not ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb):
                    return None
                return c.WorkingSetSize / 2**20
            finally:
                ctypes.windll.kernel32.CloseHandle(h)
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1024
    except OSError:
        return None
    return None


def _children(pid: int) -> list[int]:
    """Every descendant of a process (Windows: a venv's python.exe is a launcher that runs the interpreter as a
    child, so a Python process's memory is its children's)."""
    if sys.platform != "win32":
        try:
            kids = [int(x) for x in Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]
        except OSError:
            return []
        return kids + [d for k in kids for d in _children(k)]
    import ctypes
    from ctypes import wintypes

    class Entry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    k32 = ctypes.windll.kernel32
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    parent: dict[int, int] = {}
    try:
        e = Entry()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            parent[e.th32ProcessID] = e.th32ParentProcessID
            ok = k32.Process32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    out, frontier = [], [pid]
    while frontier:
        kids = [p for p, pp in parent.items() if pp in frontier and p not in out and p != pid]
        out += kids
        frontier = kids
    return out


def tree_rss_mb(pid: int) -> float | None:
    """A process's memory with its descendants'."""
    own = rss_mb(pid)
    if own is None:
        return None
    return own + sum(m for m in (rss_mb(c) for c in _children(pid)) if m is not None)


def _get(url: str, **params: Any) -> Any:
    r = requests.get(url, params=params or None, timeout=10)
    r.raise_for_status()
    return r.json()


def _check(engine: str, symbols: list[str], synced: dict[str, bool]) -> dict[str, Any]:
    out = {}
    for s in symbols:
        name = engine_name(s)
        if not synced.get(name):
            out[s] = {"synced": False}
            continue
        try:
            before = _get(f"{engine}/api/depth", symbol=name)
            binance = _get(f"{BINANCE_REST}/api/v3/depth", symbol=s, limit=10)
            after = _get(f"{engine}/api/depth", symbol=name)
        except requests.RequestException as e:
            out[s] = {"synced": True, "error": repr(e)}
            continue
        m = max((level_match(binance, before), level_match(binance, after)), key=lambda x: (x["qty"], x["price"]))
        out[s] = {"synced": True, "match": m, "crossed": crossed(before) and crossed(after)}
    return out


def _reconnects(bridge_log: Path) -> int:
    try:
        return max(0, bridge_log.read_text(encoding="utf-8", errors="replace").count("connected to Binance") - 1)
    except OSError:
        return 0


def run(symbols: list[str], hours: float, out: Path, port: int = 8096, check_s: float = 60.0,
        poll_s: float = 5.0) -> dict[str, Any]:
    out.mkdir(parents=True, exist_ok=True)
    engine = f"http://127.0.0.1:{port}"
    py = [sys.executable, "-m"]
    server = subprocess.Popen([str(_server_binary()), "--port", str(port), "--no-log"], cwd=ROOT,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    bridge_log = open(out / "bridge.out", "w")
    bridge = None
    checks: list[dict[str, Any]] = []
    resyncs: list[dict[str, Any]] = []
    lost_at: dict[str, float] = {}
    ever_synced: set[str] = set()
    died = None
    t0 = time.time()
    result: dict[str, Any] = {}

    def finish() -> dict[str, Any]:
        open_ = [{"symbol": s, "seconds": None, "lost_at_s": round(t - t0, 1)} for s, t in lost_at.items()
                 if time.time() - t > MAX_RESYNC_S]
        v = verdict(checks, resyncs + open_, _reconnects(out / "bridge.out"), (time.time() - t0) / 3600, died)
        v.update({"symbols_run": symbols, "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t0)),
                  "resync_events": resyncs})
        (out / "soak.json").write_text(json.dumps(v, indent=2), encoding="utf-8")
        return v

    try:
        _wait_up(engine)
        bridge = subprocess.Popen(py + ["research.live.feed_bridge", "--symbols", *symbols, "--engine", engine,
                                        "--status-every", "300"], cwd=ROOT, stdout=bridge_log, stderr=subprocess.STDOUT)
        next_check = time.time() + check_s
        with open(out / "soak.jsonl", "a", encoding="utf-8") as jl:
            while time.time() - t0 < hours * 3600:
                time.sleep(poll_s)
                now = time.time()
                if server.poll() is not None or bridge.poll() is not None:
                    died = "server" if server.poll() is not None else "bridge"
                    log.error("the %s stopped (exit %s)", died, (server if died == "server" else bridge).returncode)
                    break
                try:
                    feed = _get(f"{engine}/api/market-feed")["data"]["symbols"]
                except requests.RequestException as e:
                    log.warning("market-feed: %r", e)
                    continue
                synced = {name: bool(st.get("synced")) for name, st in feed.items()}
                for name, ok in synced.items():
                    if ok and name in lost_at:
                        resyncs.append({"symbol": name, "lost_at_s": round(lost_at[name] - t0, 1),
                                        "seconds": round(now - lost_at.pop(name), 1)})
                        log.info("%s resynced in %.1f s", name, resyncs[-1]["seconds"])
                    elif ok:
                        ever_synced.add(name)
                    elif name in ever_synced and name not in lost_at:
                        lost_at[name] = now
                        log.warning("%s lost sync", name)
                if now >= next_check:
                    next_check += check_s
                    row = {"t_s": round(now - t0, 1), "symbols": _check(engine, symbols, synced),
                           "memory_mb": {"server": tree_rss_mb(server.pid), "bridge": tree_rss_mb(bridge.pid)}}
                    checks.append(row)
                    jl.write(json.dumps(row) + "\n")
                    jl.flush()
                    result = finish()
                    log.info("%.2f h: %s", row["t_s"] / 3600, {s: (v.get("match") or {}).get("qty")
                                                               for s, v in row["symbols"].items()})
    finally:
        for p in (bridge, server):
            if p is not None and p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill()
        bridge_log.close()
        result = finish()
    return result


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--symbols", nargs="+", default=SYMBOLS)
    p.add_argument("--hours", type=float, default=26.0)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=8096)
    p.add_argument("--check-s", type=float, default=60.0)
    p.add_argument("--poll-s", type=float, default=5.0)
    args = p.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    v = run([s.upper() for s in args.symbols], args.hours, Path(args.out), args.port, args.check_s, args.poll_s)
    print(json.dumps({k: v[k] for k in ("pass", "criteria", "covers_24h_reconnect", "hours", "symbols")}, indent=2))
    return 0 if v["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
