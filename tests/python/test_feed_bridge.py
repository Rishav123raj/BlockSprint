"""Connector v2 (P7.2) without the network: Binance's sync rules (shared with the normalizer), each symbol's
BookSync turning crafted Binance events into /ws/market-feed messages, and those messages applied by the real
server (when it's built)."""

import asyncio
import json

import pytest

from live_server import http, needs_server, server  # noqa: F401  (server: the fixture)
from research.data.binance_sync import CONTINUE, DUPLICATE, GAP, classify, sync_from_snapshot
from research.live.feed_bridge import BookSync, engine_name


def depth(U, u, bids=(), asks=(), E=1_700_000_000_000):
    return {"e": "depthUpdate", "E": E, "U": U, "u": u, "b": [list(x) for x in bids], "a": [list(x) for x in asks]}


def snap(last_id, bids=(), asks=()):
    return {"lastUpdateId": last_id, "bids": [list(x) for x in bids], "asks": [list(x) for x in asks]}


def trade(tid, p, q, m, T=1_700_000_000_123):
    return {"e": "trade", "t": tid, "p": p, "q": q, "m": m, "T": T}


def test_the_sync_rules():
    # synced up to 100
    assert classify(100, 101, 105) == CONTINUE
    assert classify(100, 98, 103) == CONTINUE     # overlapping the start: still continues
    assert classify(100, 95, 100) == DUPLICATE
    assert classify(100, 102, 105) == GAP
    ids = lambda e: e                              # noqa: E731  (U, u) tuples
    assert sync_from_snapshot(100, [(90, 95), (96, 101), (102, 104)], ids) == (True, [(96, 101), (102, 104)])
    assert sync_from_snapshot(100, [(103, 105)], ids) == (False, [(103, 105)])   # 101-102 missing
    assert sync_from_snapshot(100, [(90, 95)], ids) == (True, [])
    assert sync_from_snapshot(100, [], ids) == (True, [])


def test_buffer_then_snapshot_then_stream():
    b = BookSync("BTC-USDT")
    assert not b.needs_snapshot
    assert b.depth(depth(98, 99, bids=[("100.00", "1")]), 1) == []
    assert b.depth(depth(100, 102, bids=[("100.00", "2")]), 2) == []
    assert b.needs_snapshot
    # Too old: the buffer starts after 50 + 1
    assert b.snapshot(snap(50, bids=[("100.00", "3")])) is None
    assert b.needs_snapshot and b.stats.snapshots_too_old == 1
    out = b.snapshot(snap(100, bids=[("100.00", "3")], asks=[("101.00", "1")]))
    assert [m["type"] for m in out] == ["snapshot", "depth"]   # 98-99 is in the snapshot; 100-102 continues it
    assert out[0] == {"type": "snapshot", "symbol": "BTC-USDT", "bids": [["100.00", "3"]], "asks": [["101.00", "1"]]}
    assert out[1]["bids"] == [["100.00", "2"]] and out[1]["ts"] == 1_700_000_000_000 * 1_000_000
    assert b.synced and b.prev_u == 102 and not b.needs_snapshot
    assert b.stats.depth_events == 2 and b.stats.depth_sent == 1

    assert b.depth(depth(103, 104, asks=[("101.00", "0")]), 3)[0]["asks"] == [["101.00", "0"]]
    assert b.depth(depth(100, 104), 4) == [] and b.stats.duplicates == 1
    assert b.depth(depth(105, 105), 5) == []        # nothing changed: nothing to send
    assert b.prev_u == 105


def test_a_gap_or_a_silence_unsyncs_until_the_next_snapshot():
    b = BookSync("ETH-USDT")
    b.depth(depth(10, 11), 1)
    b.snapshot(snap(10))
    out = b.depth(depth(20, 21, bids=[("5.00", "1")]), 2)  # 12-19 lost
    assert out == [{"type": "gap", "symbol": "ETH-USDT"}]
    assert not b.synced and b.needs_snapshot and b.stats.gaps == 1
    resync = b.snapshot(snap(20))
    assert [m["type"] for m in resync] == ["snapshot", "depth"]  # the event that showed the gap goes on top
    assert b.silence() == [{"type": "gap", "symbol": "ETH-USDT"}]
    assert b.stats.silences == 1 and not b.needs_snapshot  # waits for new events before asking for a snapshot
    assert b.silence() == []                                 # already unsynced


def test_trades_dedupe_and_name_the_aggressor():
    b = BookSync("BTC-USDT")
    first = b.trade(trade(7, "100.00", "0.5", m=True))
    assert first == [{"type": "trade", "symbol": "BTC-USDT", "ts": 1_700_000_000_123_000_000, "price": "100.00",
                      "qty": "0.5", "side": "sell"}]           # the buyer is the maker: a seller hit the bid
    assert b.trade(trade(8, "100.01", "0.1", m=False))[0]["side"] == "buy"
    assert b.trade(trade(8, "100.01", "0.1", m=False)) == []
    b.trade(trade(11, "100.02", "0.1", m=False))
    assert b.stats.trades == 3 and b.stats.trade_duplicates == 1 and b.stats.trades_missing == 2


def test_engine_names():
    assert engine_name("BTCUSDT") == "BTC-USDT"
    assert engine_name("ETHBTC") == "ETH-BTC"
    assert engine_name("BTCUSDT", plain=True) == "BTCUSDT"


@needs_server
def test_the_messages_build_the_book_in_the_server(server):
    websockets = pytest.importorskip("websockets")
    sym = "BRIDGE-USDT"
    assert http(server, "POST", "/api/symbols", {"symbol": sym, "tick_size": "0.01", "lot_size": "0.001"})["status"] == "success"
    b = BookSync(sym)
    messages = []
    messages += b.depth(depth(5, 6, bids=[("99.00000000", "0.00500000")]), 1)
    messages += b.snapshot(snap(5, bids=[("100.00000000", "0.01000000"), ("99.00000000", "0.00200000")],
                                asks=[("101.00000000", "0.00300000")]))
    messages += b.depth(depth(7, 8, bids=[("100.00000000", "0.00000000")], asks=[("100.50000000", "0.00100000")]), 2)
    messages += b.trade(trade(1, "100.50000000", "0.00100000", m=False))

    async def send():
        async with websockets.connect(f"ws://127.0.0.1:{server}/ws/market-feed") as ws:
            for m in messages:
                await ws.send(json.dumps(m))
            await asyncio.sleep(0.3)

    asyncio.run(send())
    st = http(server, "GET", "/api/market-feed")["data"]["symbols"][sym]
    assert st["synced"] and st["snapshots"] == 1 and st["depth_batches"] == 2 and st["trades"] == 1
    assert st["trade_reproduced"] == 1.0
    d = http(server, "GET", f"/api/depth?symbol={sym}")
    assert [[float(p), float(q)] for p, q in d["bids"]] == [[99.0, 0.005]]   # 100.00 removed, 99.00 set to 0.005
    assert [[float(p), float(q)] for p, q in d["asks"]] == [[101.0, 0.003]]  # 100.50 traded away
