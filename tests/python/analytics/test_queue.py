"""Queue analytics (P5.7): the level query, a hand-computed case, and the committed slice."""

from pathlib import Path

import numpy as np
import polars as pl
import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.analytics import queue, tca
from research.analytics.market import MarketData
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
E8 = 100_000_000


def test_level_qty_matches_the_bbo_timeline():
    path = str(FIXTURE)
    bbo = bs.data_bbo(path)[1:]
    assert np.array_equal(bs.data_level_qty(path, bbo["ts_ns"], np.zeros(len(bbo), np.uint8), bbo["bid"]),
                          bbo["bid_qty"])
    rev = slice(None, None, -1)  # queries in any order
    assert np.array_equal(bs.data_level_qty(path, bbo["ts_ns"][rev].copy(), np.ones(len(bbo), np.uint8),
                                            bbo["ask"][rev].copy()), bbo["ask_qty"][rev])
    t, bid = int(bbo["ts_ns"][5]), int(bbo["bid"][5])
    assert bs.data_level_qty(path, [0], [0], [bid]).tolist() == [-1]  # before the first snapshot
    assert bs.data_level_qty(path, [t], [0], [bid + 1]).tolist() == [0]  # off the tick grid
    assert bs.data_level_qty(path, [t], [1], [bid]).tolist() == [0]  # no ask at the bid
    with pytest.raises(ValueError):
        bs.data_level_qty(path, [t, t], [0], [bid])


class FakeRun:
    def __init__(self, meta, frames):
        self.meta = meta
        self._frames = frames

    def frame(self, name):
        return self._frames[name]


class FakeMarket(MarketData):
    """Fixed level quantities instead of a file replay"""

    def __init__(self, bbo, trades, levels):
        super().__init__(bbo, trades)
        self.levels = levels

    def level_qty(self, ts_ns, side, px):
        return np.array([self.levels[(int(s), round(float(p), 2))] for s, p in zip(side, px)], dtype=float)


def hand_case():
    """A: buy 1 at the best bid 100.00 behind 3, filled at t=50 after 4 traded there. B: buy 1 at 99.99, one tick
    behind, 5 ahead, cancelled unfilled. C: sell 2 at 100.01, improving the ask (100.02), 1 filled, still open."""
    bbo = np.zeros(1, dtype=bs.data_bbo(str(FIXTURE), 0, 1).dtype)
    bbo[0] = (0, 100 * E8, E8, round(100.02 * E8), E8)
    trades = np.zeros(3, dtype=bs.data_trades(str(FIXTURE), 0, 1).dtype)
    for i, (ts, px, qty, side) in enumerate([(20, 100.0, 2.0, 1), (40, 100.0, 2.0, 1), (60, 100.01, 1.0, 0)]):
        trades[i] = (ts, round(px * E8), round(qty * E8), side)
    market = FakeMarket(bbo, trades, {(0, 100.0): 3.0, (0, 99.99): 5.0, (1, 100.01): 0.0})
    requests = pl.DataFrame({
        "sent_ns": [8, 8, 28, 90], "arrive_ns": [10, 10, 30, 100], "owner": ["S1"] * 4,
        "request": ["new", "new", "new", "cancel"], "order_id": ["A", "B", "C", "B"],
        "side": ["buy", "buy", "sell", None], "order_type": ["limit_maker"] * 3 + [None],
        "price": [100.0, 99.99, 100.01, None], "quantity": [1.0, 1.0, 2.0, None]})
    cols = ("exchange_ns", "order_id", "type", "reason", "side", "price", "quantity", "last_px", "last_qty",
            "leaves_qty", "liquidity", "fee")
    rows = [(10, "A", "ack", None, "buy", 100.0, 1.0, None, None, 1.0, None, None),
            (10, "B", "ack", None, "buy", 99.99, 1.0, None, None, 1.0, None, None),
            (30, "C", "ack", None, "sell", 100.01, 2.0, None, None, 2.0, None, None),
            (50, "A", "fill", None, "buy", 100.0, 1.0, 100.0, 1.0, 0.0, "maker", 0.0),
            (60, "C", "fill", None, "sell", 100.01, 2.0, 100.01, 1.0, 1.0, "maker", 0.0),
            (100, "B", "cancelled", "user", "buy", 99.99, 1.0, None, None, 0.0, None, None)]
    reports = pl.DataFrame([dict(zip(cols, r)) for r in rows], schema={
        "exchange_ns": pl.Int64, "order_id": pl.String, "type": pl.String, "reason": pl.String, "side": pl.String,
        "price": pl.Float64, "quantity": pl.Float64, "last_px": pl.Float64, "last_qty": pl.Float64,
        "leaves_qty": pl.Float64, "liquidity": pl.String, "fee": pl.Float64}).with_columns(
        pl.lit("S1").alias("owner"), pl.lit("limit_maker").alias("order_type"))
    meta = {"window": {"start_ns": 0, "trading_ns": 0, "end_ns": 200}, "data_header": {"tick_size": "0.01"}}
    return FakeRun(meta, {"orders": requests, "exec_reports": reports}), market


def test_hand_computed_placements():
    run, md = hand_case()
    p = queue.placements(run, md)
    assert p["order_id"].to_list() == ["A", "B", "C"]
    assert p["ahead_qty"].to_list() == [3.0, 5.0, 0.0] and p["ahead_ratio"].to_list() == [3.0, 5.0, 0.0]
    assert p["ticks_from_best"].to_list() == [0, 1, -1]
    assert p["outcome"].to_list() == ["filled", "unfilled", "partial"]
    assert p["time_in_queue_ms"].to_list() == pytest.approx([40e-6, 90e-6, 30e-6])
    assert p["traded_at_price"].to_list() == [4.0, 0.0, 1.0] and p["traded_through"].to_list() == [0.0, 0.0, 0.0]
    assert p["fifo_reachable"].to_list() == [True, False, True]


def test_hand_computed_curves_and_summary():
    run, md = hand_case()
    p = queue.placements(run, md)
    ticks = queue.fill_curve(run, "ticks", placements_df=p)
    assert ticks.select("bin", "placements", "fill_rate", "full_fill_rate").rows() == [
        ("-1", 1, 1.0, 0.0), ("0", 1, 1.0, 1.0), ("1", 1, 0.0, 0.0)]
    ahead = queue.fill_curve(run, "ahead", [0, 4, 10], placements_df=p)
    assert ahead.select("bin", "placements", "fill_rate", "mean_ahead").rows() == [
        ("0 (new level)", 1, 1.0, 0.0), ("[0.0, 4.0)", 1, 1.0, 3.0), ("[4.0, 10.0)", 1, 0.0, 5.0)]
    (s,) = queue.summary(run, placements_df=p).to_dicts()
    assert s["placements"] == 3
    assert (s["at_best"], s["improved"], s["behind"]) == pytest.approx((1 / 3, 1 / 3, 1 / 3))
    assert (s["fill_rate"], s["full_fill_rate"]) == pytest.approx((2 / 3, 1 / 3))
    assert s["fifo_reachable"] == pytest.approx(2 / 3)
    assert (s["fill_rate_reachable"], s["fill_rate_unreachable"]) == (1.0, 0.0)
    assert s["median_ahead"] == 3.0
    with pytest.raises(ValueError):
        queue.fill_curve(run, "price", placements_df=p)


def test_quoter_queue_on_the_slice(tmp_path):
    res = Results.run({"dataset": str(FIXTURE), "latency_preset": "zero",
                       "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 500}},
                       "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}})
    p = queue.placements(res)
    o = tca.orders(res)
    assert p.height == o.filter(pl.col("status") != "rejected").height  # post-only: all passive
    md = MarketData.for_run(res)
    bbo = md.bbo_at(p["arrive_ns"].to_numpy())
    mask = (p["ticks_from_best"] == 0).to_numpy()
    assert mask.any()
    # Joining the best price, the queue ahead is the best level's displayed quantity
    want = np.where(p["side"].to_numpy() == "buy", bbo["bid_qty"], bbo["ask_qty"])
    assert np.allclose(p["ahead_qty"].to_numpy()[mask], want[mask])
    assert (p["time_in_queue_ms"] >= 0).all()
    assert (p.filter(pl.col("outcome") != "unfilled")["filled_qty"] > 0).all()
    curve = queue.fill_curve(res, bins=3, placements_df=p)
    assert curve["placements"].sum() == p.filter(pl.col("ahead_qty").is_not_null()).height
