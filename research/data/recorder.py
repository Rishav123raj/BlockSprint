"""Binance spot market-data recorder (BlockSprint 2.0, task P2a).

Captures, per symbol, the full diff-depth stream (``<sym>@depth@100ms``) and every trade
(``<sym>@trade``) exactly as received, plus REST depth snapshots, into the raw archive described
in :mod:`research.data.rawio`. Nothing is parsed or normalized here beyond routing frames to
their symbol, so the archive can always be re-normalized later.

Behavior:

* Each frame is stamped with ``time.time_ns()`` as soon as it is read, before JSON parsing.
* A depth snapshot (``/api/v3/depth?limit=5000``) is taken whenever a connection becomes live
  (Binance's local-book procedure needs one taken after the diff stream is flowing) and every
  ``--snapshot-interval`` seconds after that.
* Binance closes WebSocket connections after 24 h. The recorder opens a replacement after
  ``--rollover-hours`` and keeps both connections recording for an overlap window, so the switch
  loses nothing; duplicates are removed during normalization by update/trade IDs.
* Connection lifecycle (connecting, open, closed with reason) is written as ``event`` envelopes
  into every symbol's archive, so gaps are explicit in the data.
* Exchange metadata (daily) and NTP clock samples (hourly) go to the ``_meta`` archive.

Usage::

    uv run python -m research.data.recorder --symbols BTCUSDT ETHUSDT --out data/raw
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import random
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import requests
import websockets

from research.data import meta
from research.data.rawio import RawWriter, make_envelope

WS_BASE = "wss://stream.binance.com:9443/stream?streams="
META_SYMBOL = "_meta"

log = logging.getLogger("recorder")


def stream_names(symbols: list[str]) -> list[str]:
    names = []
    for s in symbols:
        names += [f"{s.lower()}@depth@100ms", f"{s.lower()}@trade"]
    return names


def build_stream_url(symbols: list[str]) -> str:
    return WS_BASE + "/".join(stream_names(symbols))


def symbol_from_stream(stream: str) -> str:
    return stream.split("@", 1)[0].upper()


@dataclass
class RecorderConfig:
    symbols: list[str]
    out_dir: Path
    snapshot_interval_s: float = 600.0
    snapshot_limit: int = 5000
    snapshot_spacing_s: float = 1.0  # limit=5000 costs 250 request weight; spread requests out
    rollover_after_s: float = 23 * 3600.0
    overlap_s: float = 30.0
    status_interval_s: float = 60.0
    clock_interval_s: float = 3600.0
    max_backoff_s: float = 60.0
    ntp_server: str = "pool.ntp.org"


class Recorder:
    def __init__(self, config: RecorderConfig, writer: RawWriter | None = None) -> None:
        self.cfg = config
        self.writer = writer or RawWriter(config.out_dir)
        self.started_ns = time.time_ns()
        self.conn_seq = 0
        self.live_connections: set[int] = set()
        self.frames = {s: 0 for s in config.symbols}
        self.snapshots = {s: 0 for s in config.symbols}
        self.last_frame_ns: dict[str, int] = {}
        self._snapshot_queue: asyncio.Queue[str] | None = None
        self._stopping = False

    # ------------------------------------------------------------------ main loop

    async def run(self) -> None:
        self._snapshot_queue = asyncio.Queue()
        tasks = [
            asyncio.create_task(self._supervise_connections(), name="connections"),
            asyncio.create_task(self._snapshot_worker(), name="snapshots"),
            asyncio.create_task(self._periodic_snapshots(), name="periodic-snapshots"),
            asyncio.create_task(self._metadata_loop(), name="metadata"),
            asyncio.create_task(self._housekeeping(), name="housekeeping"),
        ]
        try:
            await asyncio.gather(*tasks)
        finally:
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            self._event(0, "recorder_stopped")
            self.writer.close()
            self._write_status()
            log.info("recorder stopped; archive files closed")

    # ------------------------------------------------------------------ connections

    async def _supervise_connections(self) -> None:
        owned: set[asyncio.Task] = set()
        try:
            await self._connection_loop(owned)
        finally:
            # Close every connection this supervisor started before the archive is closed
            self._stopping = True
            for t in owned:
                t.cancel()
            await asyncio.gather(*owned, return_exceptions=True)

    async def _connection_loop(self, owned: set[asyncio.Task]) -> None:
        backoff = 1.0
        previous: asyncio.Task | None = None  # connection kept alive during a rollover overlap
        while True:
            self.conn_seq += 1
            conn_id = self.conn_seq
            ready = asyncio.Event()
            task = asyncio.create_task(self._connection(conn_id, ready), name=f"conn-{conn_id}")
            owned.add(task)
            task.add_done_callback(owned.discard)
            ready_wait = asyncio.create_task(ready.wait())
            done, _ = await asyncio.wait({task, ready_wait}, return_when=asyncio.FIRST_COMPLETED)

            if task in done:
                # Failed before any data arrived: retry with exponential backoff
                ready_wait.cancel()
                delay = backoff + random.uniform(0, backoff / 2)
                log.warning("connection %d failed to start (%s); retrying in %.1fs", conn_id,
                            _task_error(task), delay)
                await asyncio.sleep(delay)
                backoff = min(backoff * 2, self.cfg.max_backoff_s)
                continue

            backoff = 1.0
            # The new connection is live: fresh snapshots let its stream be synced independently
            for s in self.cfg.symbols:
                self._snapshot_queue.put_nowait(s)

            if previous is not None:
                await asyncio.sleep(self.cfg.overlap_s)  # both connections record meanwhile
                if task.done():
                    # The replacement died during the overlap: keep the old one and try again
                    continue
                previous.cancel()
                await asyncio.gather(previous, return_exceptions=True)
                previous = None

            try:
                await asyncio.wait_for(asyncio.shield(task), timeout=self.cfg.rollover_after_s)
                log.warning("connection %d closed by the server; reconnecting", conn_id)
            except asyncio.TimeoutError:
                log.info("connection %d reached %.0fs; rolling over", conn_id, self.cfg.rollover_after_s)
                previous = task
                continue
            except Exception as e:  # noqa: BLE001 - any connection failure leads to a reconnect
                log.warning("connection %d dropped (%r); reconnecting", conn_id, e)
            await asyncio.sleep(1.0)

    async def _connection(self, conn_id: int, ready: asyncio.Event) -> None:
        url = build_stream_url(self.cfg.symbols)
        self._event(conn_id, "connecting", url=url)
        reason = "closed"
        try:
            async with websockets.connect(url, open_timeout=15, close_timeout=2, max_size=2**24) as ws:
                self._event(conn_id, "open")
                self.live_connections.add(conn_id)
                log.info("connection %d open (%d streams)", conn_id, 2 * len(self.cfg.symbols))
                async for frame in ws:
                    t = time.time_ns()
                    if not ready.is_set():
                        ready.set()
                    stream = json.loads(frame)["stream"]
                    symbol = symbol_from_stream(stream)
                    self.writer.write(symbol, make_envelope(t, "ws", frame, conn=conn_id, stream=stream))
                    self.frames[symbol] += 1
                    self.last_frame_ns[symbol] = t
        except asyncio.CancelledError:
            reason = "shutdown" if self._stopping else "rollover"
            raise
        except Exception as e:
            reason = repr(e)
            raise
        finally:
            self.live_connections.discard(conn_id)
            self._event(conn_id, "closed", reason=reason)

    # ------------------------------------------------------------------ snapshots

    async def _snapshot_worker(self) -> None:
        assert self._snapshot_queue is not None
        while True:
            symbol = await self._snapshot_queue.get()
            t0 = time.time_ns()
            try:
                text = await asyncio.to_thread(self._fetch_snapshot, symbol)
                self.writer.write(symbol, make_envelope(time.time_ns(), "snapshot", text, t0=t0))
                self.snapshots[symbol] += 1
            except requests.HTTPError as e:
                status = e.response.status_code if e.response is not None else None
                self._event(0, "snapshot_error", symbols=[symbol], error=repr(e))
                # 429 = rate limited, 418 = IP banned for ignoring 429: back off hard, then retry
                delay = float(e.response.headers.get("Retry-After", 60)) if status in (418, 429) else 5.0
                log.warning("snapshot %s failed (%s); retrying in %.0fs", symbol, status, delay)
                await asyncio.sleep(delay)
                self._snapshot_queue.put_nowait(symbol)
            except Exception as e:  # noqa: BLE001
                self._event(0, "snapshot_error", symbols=[symbol], error=repr(e))
                log.warning("snapshot %s failed (%r); retrying in 5s", symbol, e)
                await asyncio.sleep(5.0)
                self._snapshot_queue.put_nowait(symbol)
            await asyncio.sleep(self.cfg.snapshot_spacing_s)

    def _fetch_snapshot(self, symbol: str) -> str:
        response = requests.get(
            f"{meta.BINANCE_REST}/api/v3/depth",
            params={"symbol": symbol, "limit": self.cfg.snapshot_limit},
            timeout=15,
        )
        response.raise_for_status()
        return response.text

    async def _periodic_snapshots(self) -> None:
        assert self._snapshot_queue is not None
        while True:
            await asyncio.sleep(self.cfg.snapshot_interval_s)
            if self.live_connections:
                for s in self.cfg.symbols:
                    self._snapshot_queue.put_nowait(s)

    # ------------------------------------------------------------------ metadata & housekeeping

    async def _metadata_loop(self) -> None:
        info_date = None
        while True:
            today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
            if today != info_date:
                try:
                    text = await asyncio.to_thread(meta.fetch_exchange_info, self.cfg.symbols)
                    self.writer.write(META_SYMBOL, make_envelope(time.time_ns(), "exchange_info", text))
                    info_date = today
                except Exception as e:  # noqa: BLE001
                    log.warning("exchangeInfo failed: %r", e)
            try:
                sample = await asyncio.to_thread(meta.sntp_sample, self.cfg.ntp_server)
                self.writer.write(META_SYMBOL, make_envelope(time.time_ns(), "clock", sample))
                log.info("clock offset %.3f ms (delay %.1f ms)", sample["offset_ns"] / 1e6, sample["delay_ns"] / 1e6)
            except Exception as e:  # noqa: BLE001
                log.warning("NTP sample failed: %r", e)
            await asyncio.sleep(self.cfg.clock_interval_s)

    async def _housekeeping(self) -> None:
        last_status = 0.0
        while True:
            await asyncio.sleep(1.0)
            self.writer.maybe_flush()
            now = time.monotonic()
            if now - last_status >= self.cfg.status_interval_s:
                last_status = now
                self._write_status()
                log.info("frames %s | snapshots %s | live connections %s",
                         self.frames, self.snapshots, sorted(self.live_connections))

    def _event(self, conn_id: int, name: str, **info: object) -> None:
        """Lifecycle event, written to every symbol's archive so each stream records its own gaps."""
        payload = {"event": name, **info}
        t = time.time_ns()
        for s in self.cfg.symbols:
            self.writer.write(s, make_envelope(t, "event", payload, conn=conn_id or None))

    def _write_status(self) -> None:
        status = {
            "updated_ns": time.time_ns(),
            "started_ns": self.started_ns,
            "live_connections": sorted(self.live_connections),
            "symbols": {
                s: {"frames": self.frames[s], "snapshots": self.snapshots[s],
                    "last_frame_ns": self.last_frame_ns.get(s)}
                for s in self.cfg.symbols
            },
        }
        path = Path(self.cfg.out_dir) / "_status.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(status, indent=2))
        tmp.replace(path)


def _task_error(task: asyncio.Task) -> str:
    if task.cancelled():
        return "cancelled"
    exc = task.exception()
    return repr(exc) if exc else "closed"


def validate_symbols(symbols: list[str]) -> None:
    """Fail fast on unknown or non-trading symbols."""
    try:
        statuses = meta.trading_symbols(meta.fetch_exchange_info(symbols))
    except requests.HTTPError as e:
        body = e.response.text if e.response is not None else ""
        raise SystemExit(f"Binance rejected the symbol list {symbols}: {body}") from e
    not_trading = {s: st for s, st in statuses.items() if st != "TRADING"}
    if not_trading:
        raise SystemExit(f"Symbols not trading: {not_trading}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Record Binance spot diff-depth and trades to a raw archive.")
    p.add_argument("--symbols", nargs="+", required=True, help="Binance symbols, e.g. BTCUSDT ETHUSDT")
    p.add_argument("--out", type=Path, default=Path("data/raw"), help="archive root (default: data/raw)")
    p.add_argument("--snapshot-interval", type=float, default=600.0, help="seconds between depth snapshots")
    p.add_argument("--rollover-hours", type=float, default=23.0, help="replace connections after this long")
    p.add_argument("--overlap-seconds", type=float, default=30.0,
                   help="how long old and new connections record together during a rollover")
    p.add_argument("--duration", type=float, default=None, help="stop after this many seconds (testing)")
    p.add_argument("--log-file", type=Path, default=Path("logs/recorder.log"))
    p.add_argument("--no-validate", action="store_true", help="skip the exchangeInfo symbol check")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    args.log_file.parent.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=[logging.StreamHandler(sys.stdout), logging.FileHandler(args.log_file, encoding="utf-8")],
    )
    symbols = [s.upper() for s in args.symbols]
    if not args.no_validate:
        validate_symbols(symbols)

    config = RecorderConfig(symbols=symbols, out_dir=args.out, snapshot_interval_s=args.snapshot_interval,
                            rollover_after_s=args.rollover_hours * 3600, overlap_s=args.overlap_seconds)
    recorder = Recorder(config)
    log.info("recording %s to %s", symbols, args.out.resolve())

    async def run() -> None:
        if args.duration is None:
            await recorder.run()
        else:
            try:
                await asyncio.wait_for(recorder.run(), timeout=args.duration)
            except asyncio.TimeoutError:
                log.info("duration of %.0fs reached", args.duration)

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        log.info("interrupted")


if __name__ == "__main__":
    main()
