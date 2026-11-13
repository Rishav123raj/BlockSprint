"""Connector v2 (P7.2): Binance's diff-depth and trade streams, kept exact with the normalizer's sync rules,
sent to the server's /ws/market-feed (P7.1), where the simulator's Synthesizer turns them into the book.

v1 (scripts/binance_connector.py --v1) mirrored each level of the top-10 partial book as one order and
re-submitted a level on every change, which put it at the back of the queue (D10), and its frames had no
sequence numbers, so a lost one went unnoticed. v2 streams every update with its IDs:

* per symbol ``<sym>@depth@100ms`` (every change, with update IDs) and ``<sym>@trade``, on one connection;
* depth events are buffered until a REST snapshot (``/api/v3/depth?limit=1000``) and then applied with
  ``research.data.binance_sync`` (the normalizer's rules): the server gets the snapshot, then each depth event
  as absolute quantities; a duplicate is dropped; a gap, a silence over ``stale_after_s`` or a lost connection
  sends ``gap`` and the symbol resyncs from the next snapshot;
* trades go through as they come (deduplicated by trade ID); the aggressor is the seller when ``m`` (the buyer
  is the maker) is true.

Usage::

    uv run python -m research.live.feed_bridge --symbols BTCUSDT ETHUSDT
    uv run python -m research.live.feed_bridge --symbols BTCUSDT --duration 60 --engine http://127.0.0.1:8080

Symbols are registered in the engine with Binance's rules under the 1.0 names (``BTC-USDT``) unless
``--plain-names``; ``--no-reset`` keeps the engine's state.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any

import requests

from research.data.binance_sync import CONTINUE, DUPLICATE, GAP, classify, sync_from_snapshot

log = logging.getLogger("feed_bridge")

BINANCE_REST = "https://api.binance.com"
BINANCE_WS = "wss://stream.binance.com:9443/stream"


def engine_name(symbol: str, plain: bool = False) -> str:
    """The engine's name for a Binance symbol: BTC-USDT (as 1.0's connector registered it), or BTCUSDT."""
    if plain:
        return symbol
    for quote in ("USDT", "USDC", "FDUSD", "BTC", "ETH", "BNB"):
        if symbol.endswith(quote) and len(symbol) > len(quote):
            return f"{symbol[: -len(quote)]}-{quote}"
    return symbol


@dataclass
class SyncStats:
    depth_events: int = 0
    depth_sent: int = 0
    duplicates: int = 0
    gaps: int = 0
    silences: int = 0
    snapshots: int = 0
    snapshots_too_old: int = 0
    trades: int = 0
    trade_duplicates: int = 0
    trades_missing: int = 0


@dataclass
class BookSync:
    """One symbol's stream state; each call returns the /ws/market-feed messages to send, in order. No I/O."""
    symbol: str                      # the engine's name
    buffer_limit: int = 10_000
    synced: bool = False
    prev_u: int = 0
    buffer: list[dict[str, Any]] = field(default_factory=list)
    last_trade_id: int | None = None
    last_depth_ns: int = 0           # local time of the last depth event (for the silence check)
    stats: SyncStats = field(default_factory=SyncStats)

    @property
    def needs_snapshot(self) -> bool:
        return not self.synced and bool(self.buffer)

    def _depth_message(self, e: dict[str, Any]) -> dict[str, Any] | None:
        if not e["b"] and not e["a"]:
            return None
        return {"type": "depth", "symbol": self.symbol, "ts": e["E"] * 1_000_000, "bids": e["b"], "asks": e["a"]}

    def _gap(self) -> dict[str, Any]:
        self.synced = False
        self.stats.gaps += 1
        return {"type": "gap", "symbol": self.symbol}

    def depth(self, e: dict[str, Any], now_ns: int) -> list[dict[str, Any]]:
        """A ``depthUpdate`` event (``U``, ``u``, ``b``, ``a``, ``E``)."""
        self.stats.depth_events += 1
        self.last_depth_ns = now_ns
        if not self.synced:
            self.buffer.append(e)
            if len(self.buffer) > self.buffer_limit:
                del self.buffer[: len(self.buffer) - self.buffer_limit]
            return []
        what = classify(self.prev_u, e["U"], e["u"])
        if what == DUPLICATE:
            self.stats.duplicates += 1
            return []
        if what == GAP:
            self.buffer = [e]
            return [self._gap()]
        assert what == CONTINUE
        self.prev_u = e["u"]
        m = self._depth_message(e)
        if m is None:
            return []
        self.stats.depth_sent += 1
        return [m]

    def snapshot(self, snap: dict[str, Any]) -> list[dict[str, Any]] | None:
        """A REST depth snapshot (``lastUpdateId``, ``bids``, ``asks``): the snapshot and the buffered events after
        it, or None if it's older than the buffer (wait for another)."""
        ok, pending = sync_from_snapshot(snap["lastUpdateId"], self.buffer, lambda e: (e["U"], e["u"]))
        if not ok:
            self.buffer = pending
            self.stats.snapshots_too_old += 1
            return None
        self.stats.snapshots += 1
        self.synced = True
        self.prev_u = snap["lastUpdateId"]
        self.buffer = []
        out = [{"type": "snapshot", "symbol": self.symbol, "bids": snap["bids"], "asks": snap["asks"]}]
        for e in pending:
            out += self.depth(e, self.last_depth_ns)
            self.stats.depth_events -= 1  # counted when buffered
        return out

    def silence(self) -> list[dict[str, Any]]:
        """No depth event for too long (or the connection dropped): the book can't be trusted any more."""
        self.buffer = []
        if not self.synced:
            return []
        self.stats.silences += 1
        return [self._gap()]

    def trade(self, t: dict[str, Any]) -> list[dict[str, Any]]:
        """A ``trade`` event (``t`` id, ``p``, ``q``, ``m``, ``T``)."""
        tid = t["t"]
        if self.last_trade_id is not None and tid <= self.last_trade_id:
            self.stats.trade_duplicates += 1
            return []
        if self.last_trade_id is not None and tid > self.last_trade_id + 1:
            self.stats.trades_missing += tid - self.last_trade_id - 1
        self.last_trade_id = tid
        self.stats.trades += 1
        return [{"type": "trade", "symbol": self.symbol, "ts": t["T"] * 1_000_000, "price": t["p"], "qty": t["q"],
                 "side": "sell" if t["m"] else "buy"}]


class FeedBridge:
    """Binance in, /ws/market-feed out. One Binance connection for every symbol; snapshots one at a time."""

    def __init__(self, symbols: list[str], engine: str = "http://127.0.0.1:8080", plain_names: bool = False,
                 reset: bool = True, snapshot_limit: int = 1000, stale_after_s: float = 10.0,
                 status_every_s: float = 30.0):
        self.symbols = [s.upper() for s in symbols]
        self.engine = engine.rstrip("/")
        self.names = {s: engine_name(s, plain_names) for s in self.symbols}
        self.reset = reset
        self.snapshot_limit = snapshot_limit
        self.stale_ns = int(stale_after_s * 1e9)
        self.status_every_s = status_every_s
        self.books = {s: BookSync(self.names[s]) for s in self.symbols}
        self.errors: list[str] = []
        self._snapshot_wanted: set[str] = set()

    # ---- the engine side ----

    def prepare_engine(self) -> None:
        if self.reset:
            requests.post(f"{self.engine}/api/reset", timeout=5).raise_for_status()
        info = requests.get(f"{BINANCE_REST}/api/v3/exchangeInfo",
                            params={"symbols": json.dumps(self.symbols, separators=(",", ":"))}, timeout=10)
        info.raise_for_status()
        for s in info.json()["symbols"]:
            f = {x["filterType"]: x for x in s["filters"]}
            lot = f.get("LOT_SIZE", {})
            notional = f.get("NOTIONAL") or f.get("MIN_NOTIONAL") or {}
            spec = {"symbol": self.names[s["symbol"]], "tick_size": f.get("PRICE_FILTER", {}).get("tickSize", "0"),
                    "lot_size": lot.get("stepSize", "0"), "min_qty": lot.get("minQty", "0"),
                    "max_qty": lot.get("maxQty", "0"), "min_notional": notional.get("minNotional", "0")}
            requests.post(f"{self.engine}/api/symbols", json=spec, timeout=5).raise_for_status()
            log.info("%s: tick %s, lot %s", spec["symbol"], spec["tick_size"], spec["lot_size"])

    def feed_url(self) -> str:
        return self.engine.replace("http://", "ws://").replace("https://", "wss://") + "/ws/market-feed"

    async def _send(self, ws, messages: list[dict[str, Any]]) -> None:
        if messages:
            await ws.send(json.dumps(messages if len(messages) > 1 else messages[0]))

    async def _read_errors(self, ws) -> None:
        async for frame in ws:
            msg = json.loads(frame)
            self.errors.append(msg.get("message", frame))
            log.warning("engine: %s", msg.get("message", frame))

    # ---- Binance ----

    def _fetch_snapshot(self, symbol: str) -> dict[str, Any]:
        r = requests.get(f"{BINANCE_REST}/api/v3/depth", params={"symbol": symbol, "limit": self.snapshot_limit},
                         timeout=15)
        r.raise_for_status()
        return r.json()

    async def _snapshots(self, ws) -> None:
        """Takes a snapshot for each symbol that has buffered events and no book, one at a time."""
        while True:
            wanted = [s for s in self.symbols if self.books[s].needs_snapshot]
            if not wanted:
                await asyncio.sleep(0.2)
                continue
            for s in wanted:
                try:
                    snap = await asyncio.to_thread(self._fetch_snapshot, s)
                except requests.HTTPError as e:
                    status = e.response.status_code if e.response is not None else None
                    delay = float(e.response.headers.get("Retry-After", 60)) if status in (418, 429) else 5.0
                    log.warning("snapshot %s failed (%s); retrying in %.0f s", s, status, delay)
                    await asyncio.sleep(delay)
                    continue
                out = self.books[s].snapshot(snap)
                if out is None:
                    log.info("%s: snapshot %d older than the buffered updates; taking another", s, snap["lastUpdateId"])
                else:
                    log.info("%s: synced at update %d", s, snap["lastUpdateId"])
                    await self._send(ws, out)
                await asyncio.sleep(1.0)  # limit=1000 weighs 50: keep well under 6000 a minute

    async def _watchdog(self, ws) -> None:
        while True:
            await asyncio.sleep(1.0)
            now = time.time_ns()
            for s in self.symbols:
                b = self.books[s]
                if b.synced and now - b.last_depth_ns > self.stale_ns:
                    log.warning("%s: no depth update for %.0f s: gap, resyncing", s, (now - b.last_depth_ns) / 1e9)
                    await self._send(ws, b.silence())

    async def _status(self) -> None:
        while True:
            await asyncio.sleep(self.status_every_s)
            log.info("status %s", self.status())

    def status(self) -> dict[str, Any]:
        out = {}
        try:
            engine = requests.get(f"{self.engine}/api/market-feed", timeout=3).json()["data"]["symbols"]
        except Exception:  # noqa: BLE001
            engine = {}
        for s in self.symbols:
            b = self.books[s]
            e = engine.get(b.symbol, {})
            out[s] = {"synced": b.synced, "depth": b.stats.depth_sent, "trades": b.stats.trades, "gaps": b.stats.gaps,
                      "engine_synced": e.get("synced"), "trade_reproduced": e.get("trade_reproduced"),
                      "top": e.get("top")}
        return out

    async def run(self, duration_s: float | None = None) -> None:
        import websockets

        streams = "/".join(f"{s.lower()}@depth@100ms/{s.lower()}@trade" for s in self.symbols)
        url = f"{BINANCE_WS}?streams={streams}"
        await asyncio.to_thread(self.prepare_engine)
        deadline = None if duration_s is None else time.monotonic() + duration_s
        async with websockets.connect(self.feed_url(), max_size=None) as feed:
            helpers = [asyncio.create_task(self._read_errors(feed)), asyncio.create_task(self._snapshots(feed)),
                       asyncio.create_task(self._watchdog(feed)), asyncio.create_task(self._status())]
            try:
                while deadline is None or time.monotonic() < deadline:
                    try:
                        await self._pump(url, feed, deadline)
                    except (OSError, websockets.ConnectionClosed) as e:
                        log.warning("Binance connection lost (%r): gaps, reconnecting in 3 s", e)
                        for s in self.symbols:
                            await self._send(feed, self.books[s].silence())
                        await asyncio.sleep(3)
            finally:
                for t in helpers:
                    t.cancel()
                await asyncio.gather(*helpers, return_exceptions=True)

    async def _pump(self, url: str, feed, deadline: float | None) -> None:
        import websockets

        async with websockets.connect(url, max_size=None, ping_interval=20) as binance:
            log.info("connected to Binance: %d symbols", len(self.symbols))
            while deadline is None or time.monotonic() < deadline:
                timeout = None if deadline is None else max(0.0, deadline - time.monotonic())
                try:
                    frame = await asyncio.wait_for(binance.recv(), timeout)
                except asyncio.TimeoutError:
                    return
                now = time.time_ns()
                msg = json.loads(frame)
                data = msg.get("data", {})
                stream = msg.get("stream", "")
                book = self.books.get(stream.split("@")[0].upper())
                if book is None:
                    continue
                if data.get("e") == "depthUpdate":
                    await self._send(feed, book.depth(data, now))
                elif data.get("e") == "trade":
                    await self._send(feed, book.trade(data))


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--symbols", nargs="+", default=["BTCUSDT", "ETHUSDT"])
    p.add_argument("--engine", default="http://127.0.0.1:8080")
    p.add_argument("--plain-names", action="store_true", help="register BTCUSDT, not BTC-USDT")
    p.add_argument("--no-reset", action="store_true", help="don't reset the engine first")
    p.add_argument("--duration", type=float, default=None, help="stop after this many seconds")
    p.add_argument("--status-every", type=float, default=30.0)
    args = p.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    bridge = FeedBridge(args.symbols, args.engine, args.plain_names, not args.no_reset,
                        status_every_s=args.status_every)
    try:
        asyncio.run(bridge.run(args.duration))
    except KeyboardInterrupt:
        pass
    log.info("final status %s", json.dumps(bridge.status()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
