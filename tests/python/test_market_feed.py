"""The live server's /ws/market-feed (P7.1) end to end: the real server binary, fed over a WebSocket, builds
synthetic queues through the simulator's Synthesizer, and a client order resting behind them fills only when
the trades have reached it. Skipped when the server isn't built (the Python-only CI job)."""

import asyncio
import json

import pytest

from live_server import http, needs_server, server  # noqa: F401  (server: the fixture)

websockets = pytest.importorskip("websockets")
pytestmark = needs_server
SYMBOL = "FEEDUSDT"


async def scenario(port):
    ws = f"ws://127.0.0.1:{port}"
    async with websockets.connect(ws + "/ws/market-feed") as feed, \
            websockets.connect(ws + "/ws/orders") as orders, \
            websockets.connect(ws + "/ws/executions") as execs, \
            websockets.connect(ws + "/ws/market-data") as md:

        async def stats():
            await asyncio.sleep(0.2)  # the feed doesn't reply on success: let the server apply it
            return http(port, "GET", "/api/market-feed")["data"]["symbols"][SYMBOL]

        await feed.send(json.dumps({"type": "snapshot", "symbol": SYMBOL,
                                    "bids": [["100.00", "0.010"]], "asks": [["101.00", "0.004"]]}))
        assert (await stats())["synced"] is True
        depth = http(port, "GET", f"/api/depth?symbol={SYMBOL}")
        assert [float(x) for x in depth["bids"][0]] == [100.0, 0.01]  # [price, qty]

        # A client bid joins behind the 10 synthetic lots
        await orders.send(json.dumps({"symbol": SYMBOL, "order_type": "limit", "side": "buy", "price": "100.00",
                                      "quantity": "0.002", "order_id": "c1", "owner": "C1"}))
        assert json.loads(await orders.recv())["status"] == "success"

        # 9 lots trade: still ahead of us. Then the next 3: 1 synthetic, then our 2.
        await feed.send(json.dumps([{"type": "trade", "symbol": SYMBOL, "side": "sell", "price": "100.00", "qty": "0.009"},
                                    {"type": "depth", "symbol": SYMBOL, "bids": [["100.00", "0.001"]]}]))
        st = await stats()
        assert st["trades"] == 1 and st["trade_reproduced"] == 1.0
        await feed.send(json.dumps({"type": "trade", "symbol": SYMBOL, "side": "sell", "price": "100.00", "qty": "0.003"}))
        st = await stats()
        assert st["trade_diverted_qty"] == "0.002" and st["trade_reproduced"] == 0.5

        # The executions stream carries the client's reports only (not the mirror's synthetic orders)
        seen = []
        while True:
            msg = json.loads(await asyncio.wait_for(execs.recv(), 2))
            seen.append(msg["data"])
            if msg["data"]["type"] == "fill":
                break
        assert {r["owner"] for r in seen} == {"C1"}
        fill = seen[-1]
        assert fill["order_id"] == "c1" and fill["last_qty"] == "0.002" and fill["liquidity"] == "maker"

        # A gap is passed on to market-data subscribers; errors come back to the connector
        await feed.send(json.dumps({"type": "gap", "symbol": SYMBOL}))
        while True:
            msg = json.loads(await asyncio.wait_for(md.recv(), 2))
            if msg.get("event") == "gap":
                assert msg["symbol"] == SYMBOL
                break
        assert (await stats())["synced"] is False
        await feed.send(json.dumps({"type": "depth", "symbol": SYMBOL, "bids": [["100.005", "0.001"]]}))
        err = json.loads(await asyncio.wait_for(feed.recv(), 2))
        assert err["status"] == "error" and "multiple of the tick" in err["message"]


def test_the_feed_builds_queues_that_fill_a_client_in_order(server):
    assert http(server, "POST", "/api/symbols", {"symbol": SYMBOL, "tick_size": "0.01", "lot_size": "0.001"})["status"] == "success"
    asyncio.run(scenario(server))
    # The tape owner is reserved
    r = http(server, "POST", "/api/orders", {"symbol": SYMBOL, "order_type": "limit", "side": "buy", "price": "99.00",
                                             "quantity": "0.001", "owner": "MARKET_TAPE"})
    assert r["status"] == "error" and "reserved" in r["message"]


def test_reset_forgets_the_feed(server):
    http(server, "POST", "/api/symbols", {"symbol": SYMBOL, "tick_size": "0.01", "lot_size": "0.001"})

    async def feed_one():
        async with websockets.connect(f"ws://127.0.0.1:{server}/ws/market-feed") as feed:
            await feed.send(json.dumps({"type": "snapshot", "symbol": SYMBOL, "bids": [["100.00", "0.005"]], "asks": []}))
            await asyncio.sleep(0.2)

    asyncio.run(feed_one())
    assert SYMBOL in http(server, "GET", "/api/market-feed")["data"]["symbols"]
    http(server, "POST", "/api/reset")
    assert http(server, "GET", "/api/market-feed")["data"]["symbols"] == {}
