"""The live strategy runner (P7.3): research strategies run unchanged in paper mode. Unit tests of the pieces
(conversions, the book without the strategy's own orders, reports and refusals) and the research PassiveQuoter,
the same class the simulator runs, against the real server with a scripted market feed."""

import asyncio
import json

import pytest

import blocksprint_sim as bs
from live_server import http, needs_server, server  # noqa: F401  (server: the fixture)
from research.live.runner import (Account, ExecReport, LiveBook, LiveRunner, decimal_text, fixed, iso_ns, notional)
from research.strategies import Strategy
from research.strategies.passive_quoter import PassiveQuoter

LOT, TICK = 100_000, 1_000_000  # 0.001, 0.01


def test_conversions():
    assert fixed("68909.650000") == 6_890_965_000_000
    assert fixed("0.00001000") == 1000 and fixed("") == 0
    assert decimal_text(100_000) == "0.00100000" and decimal_text(-150_000_000) == "-1.50000000"
    assert iso_ns("2026-11-13T18:36:27.833329Z") == 1_794_594_987_833_329_000
    assert notional(10_000 * TICK, 2 * LOT) == 20_000_000           # 100.00 x 0.002 = 0.2
    assert notional(3, 50_000_000) == 2 and notional(1, 50_000_000) == 1   # half away from zero


def test_account_follows_fills_like_the_simulators():
    a = Account()
    fill = ExecReport.from_json({"type": "fill", "side": "buy", "last_px": "100.0", "last_qty": "0.002", "fee": "0.0002",
                                 "liquidity": "maker", "order_id": "x", "symbol": "S"})
    a.apply_fill(fill)
    a.apply_fill(ExecReport.from_json({"type": "fill", "side": "sell", "last_px": "101.0", "last_qty": "0.001",
                                       "fee": "0.000101", "liquidity": "taker"}))
    # the simulator's accounts test (gateway_test.cpp): the same numbers
    assert (a.position, a.fees, a.fills, a.maker_fills, a.taker_fills) == (100_000, 30_100, 2, 1, 1)
    assert a.cash == -20_000_000 + 10_100_000
    assert a.pnl(102 * 100_000_000) == 269_900


def test_report_fields_and_enums():
    r = ExecReport.from_json({"seq": 7, "timestamp": "2026-11-13T18:36:27.000000001Z", "type": "rejected",
                              "reason": "post_only_would_take", "text": "x", "order_id": "S1-1", "owner": "S1",
                              "symbol": "BTC-USDT", "side": "sell", "order_type": "limit_maker", "price": "100.1",
                              "quantity": "0.001", "cum_qty": "0.0", "leaves_qty": "0.0", "cum_fee": "0.0"})
    assert r.type == bs.ExecType.REJECTED and r.reason == bs.ExecReason.POST_ONLY_WOULD_TAKE
    assert r.order_type == bs.OrderType.LIMIT_MAKER and r.side == bs.Side.SELL and r.owner_id == "S1"
    assert r.price == 10_010 * TICK and r.timestamp % 1_000_000_000 == 1


def test_the_book_hides_the_strategys_own_orders_and_waits_out_a_gap():
    b = LiveBook("BTC-USDT", 0, TICK, LOT)
    assert not b.valid() and b.best_bid() is None
    depth = {"bids": [["100.000000", "0.006000"], ["99.990000", "0.001000"]], "asks": [["100.010000", "0.002000"]]}
    b.update(depth, {(bs.Side.BUY, 10_000 * TICK): LOT, (bs.Side.BUY, 9_999 * TICK): LOT}, 5)
    assert b.valid() and b.best_bid().qty == 5 * LOT and b.levels(bs.Side.BUY) == 1  # 99.99 was all ours
    assert b.qty(bs.Side.SELL, 10_001 * TICK) == 2 * LOT and b.data_time() == 5
    b.gap()
    assert not b.valid()
    b.update({"bids": [["100.000000", "0.001000"]], "asks": []}, {(bs.Side.BUY, 10_000 * TICK): LOT}, 6)
    assert not b.valid()        # only the strategy's own order is left: the feed hasn't resynced
    b.update(depth, {}, 7)
    assert b.valid() and b.top(bs.Side.BUY, 5)[0].qty == 6 * LOT


class Recorder(Strategy):
    def __init__(self):
        self.log = []

    def on_exec(self, ctx, report):
        self.log.append(("exec", report.type, report.order_id))

    def on_reject(self, ctx, reject):
        self.log.append(("reject", reject.request, reject.order_id, reject.text))


def offline_runner(strategy):
    """A runner without sockets: requests go to a queue the test reads."""
    r = LiveRunner(strategy, ["T-USDT"], owner="S9", id_prefix="S9")
    r.books["T-USDT"] = LiveBook("T-USDT", 0, TICK, LOT)
    r._outbox = asyncio.Queue()
    return r


def test_replies_and_reports_become_the_simulators_callbacks():
    s = Recorder()
    r = offline_runner(s)
    oid = r.send("T-USDT", bs.Side.BUY, bs.OrderType.LIMIT, 10_000 * TICK, LOT, bs.StpPolicy.CANCEL_MAKER)
    assert oid == "S9-1" and r.orders[oid].status == bs.OrderStatus.PENDING_NEW
    sent = json.loads(r._outbox.get_nowait())
    assert sent == {"action": "submit", "symbol": "T-USDT", "order_id": "S9-1", "owner": "S9", "side": "buy",
                    "order_type": "limit", "quantity": "0.00100000", "stp": "cancel_maker", "price": "100.00000000"}
    report = {"type": "ack", "order_id": oid, "owner": "S9", "symbol": "T-USDT", "side": "buy", "order_type": "limit",
              "price": "100.0", "quantity": "0.001", "cum_qty": "0.0", "leaves_qty": "0.001"}
    r._on_execution({**report, "owner": "SOMEONE"})                  # another owner's: ignored
    r._on_execution(report)
    r._on_order_reply({"status": "success", "reports": [report]})
    assert r.orders[oid].status == bs.OrderStatus.NEW

    # An amend the engine refuses: a REJECTED report for a live order is an on_reject, the order stays
    r.amend(oid, 2 * LOT, 9_000_000)                                  # 0.09: off the 0.01 grid
    r._on_execution({**report, "type": "rejected", "reason": "invalid_price", "text": "bad tick"})
    assert s.log[-1] == ("reject", bs.Request.AMEND, oid, "bad tick") and oid in r.orders

    # A cancel of an order no longer there
    r.cancel("S9-404")
    r._on_order_reply({"status": "success", "reports": []})          # the amend's reply
    r._on_order_reply({"status": "error", "message": "Order not found or already executed"})
    assert s.log[-1] == ("reject", bs.Request.CANCEL, "S9-404", "Order not found or already executed")

    # A replace of an order not known as open is refused here, as the simulator's gateway refuses it
    r.replace("S9-404", 10_000 * TICK, LOT)
    assert s.log[-1][:2] == ("reject", bs.Request.REPLACE) and r.stats["local_refusals"] == 1

    # A fill, then the end of the order
    r._on_execution({**report, "type": "fill", "last_px": "100.0", "last_qty": "0.001", "cum_qty": "0.001",
                     "leaves_qty": "0.0", "liquidity": "maker", "fee": "0.0001"})
    assert oid not in r.orders and r.accounts["T-USDT"].position == LOT
    assert [e[1] for e in s.log if e[0] == "exec"] == [bs.ExecType.ACK, bs.ExecType.FILL]


@needs_server
def test_the_research_quoter_runs_unchanged_in_paper_mode(server, tmp_path):
    websockets = pytest.importorskip("websockets")
    sym = "PAPER-USDT"
    assert http(server, "POST", "/api/symbols", {"symbol": sym, "tick_size": "0.01", "lot_size": "0.001"})["status"] == "success"
    quoter = PassiveQuoter(size="0.001", refresh_ms=100, max_position="0.005")  # the class the simulator runs
    runner = LiveRunner(quoter, [sym], engine=f"http://127.0.0.1:{server}", owner="P1", out_dir=tmp_path,
                        sample_ms=200)

    async def feed():
        async with websockets.connect(f"ws://127.0.0.1:{server}/ws/market-feed") as ws:
            await ws.send(json.dumps({"type": "snapshot", "symbol": sym, "bids": [["100.00", "0.005"]],
                                      "asks": [["100.10", "0.005"]]}))
            await asyncio.sleep(1.0)  # the quoter joins both sides, behind 5 each
            # A seller takes the 5 ahead and our 1; then a buyer takes the asks ahead and ours
            await ws.send(json.dumps([
                {"type": "trade", "symbol": sym, "side": "sell", "price": "100.00", "qty": "0.006"},
                {"type": "depth", "symbol": sym, "bids": [["100.00", "0.0"], ["99.99", "0.004"]]}]))
            await asyncio.sleep(0.5)
            await ws.send(json.dumps([
                {"type": "trade", "symbol": sym, "side": "buy", "price": "100.10", "qty": "0.006"},
                {"type": "depth", "symbol": sym, "asks": [["100.10", "0.0"], ["100.11", "0.004"]]}]))
            await asyncio.sleep(1.0)

    async def both():
        result = asyncio.create_task(runner.run(duration_s=3.0))
        await asyncio.sleep(0.3)
        await feed()
        return await result

    meta = asyncio.run(both())
    acct = meta["accounts"][sym]
    journal = (tmp_path / "session.jsonl").read_text()
    assert acct["fills"] == 2, [x for x in journal.splitlines() if '"sample"' not in x]
    assert acct["maker_fills"] == 2 and acct["position"] == 0
    assert acct["cash"] == 10_000 and acct["fees"] == 20_010   # sold for 0.1001, bought for 0.1; 0.1% of each
    assert meta["stats"]["timers"] >= 20 and meta["stats"]["requests"] >= 2
    assert meta["open_orders"] == 0                       # cancelled at the end
    lines = [json.loads(x) for x in (tmp_path / "session.jsonl").read_text().splitlines()]
    kinds = {x["kind"] for x in lines}
    assert {"start", "request", "report", "sample", "end"} <= kinds
    fills = [x for x in lines if x["kind"] == "report" and x.get("type") == "fill"]
    assert [(f["side"], f["last_px"], f["liquidity"]) for f in fills] == [("buy", "100.0", "maker"), ("sell", "100.1", "maker")]
    # The quoter saw the market without its own orders: its quotes joined at 100.00 / 100.10, behind 5 each
    first_bid = next(x for x in lines if x["kind"] == "request" and x.get("side") == "buy")
    assert first_bid["price"] == "100.00000000" and first_bid["order_type"] == "limit_maker"
    assert (tmp_path / "session_meta.json").exists()
