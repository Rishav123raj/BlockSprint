"""Transaction cost analysis (P5.5): a hand-computed case, and consistency with the simulator's own counts."""

from pathlib import Path

import numpy as np
import polars as pl
import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.analytics import tca
from research.analytics.market import MarketData, table
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
E8 = 100_000_000


class FakeRun:
    def __init__(self, meta, frames):
        self.meta = meta
        self._frames = frames

    def frame(self, name):
        return self._frames[name]


def market():
    # The mid: 100 from t=0, 101 from t=100, 102 from t=180
    bbo = np.zeros(3, dtype=bs.data_bbo(str(FIXTURE), 0, 1).dtype)
    for i, (ts, bid, ask) in enumerate([(0, 99.99, 100.01), (100, 100.99, 101.01), (180, 101.99, 102.01)]):
        bbo[i] = (ts, round(bid * E8), E8, round(ask * E8), E8)
    trades = np.zeros(3, dtype=bs.data_trades(str(FIXTURE), 0, 1).dtype)
    for i, (ts, px, qty, side) in enumerate([(2, 100.01, 3.0, 0), (120, 101.0, 2.0, 1), (250, 103.0, 1.0, 0)]):
        trades[i] = (ts, round(px * E8), round(qty * E8), side)
    return MarketData(bbo, trades)


def _requests(rows):
    cols = ("sent_ns", "arrive_ns", "owner", "request", "order_id", "side", "order_type", "price", "quantity")
    return pl.DataFrame([dict(zip(cols, r)) for r in rows], schema={
        "sent_ns": pl.Int64, "arrive_ns": pl.Int64, "owner": pl.String, "request": pl.String, "order_id": pl.String,
        "side": pl.String, "order_type": pl.String, "price": pl.Float64, "quantity": pl.Float64})


def _reports(rows):
    cols = ("exchange_ns", "order_id", "type", "reason", "side", "order_type", "price", "quantity", "last_px",
            "last_qty", "leaves_qty", "liquidity", "fee")
    df = pl.DataFrame([dict(zip(cols, r)) for r in rows], schema={
        "exchange_ns": pl.Int64, "order_id": pl.String, "type": pl.String, "reason": pl.String, "side": pl.String,
        "order_type": pl.String, "price": pl.Float64, "quantity": pl.Float64, "last_px": pl.Float64,
        "last_qty": pl.Float64, "leaves_qty": pl.Float64, "liquidity": pl.String, "fee": pl.Float64})
    return df.with_columns(pl.lit("S1").alias("owner"))


def hand_case():
    """B: a market buy of 2, filled at once. S: a sell quote of 1 at 101.01, replaced at 101.02, 0.4 filled, then
    cancelled. R: a buy refused for the open-order limit. Then a cancel refused at t=180."""
    requests = _requests([
        (0, 5, "S1", "new", "B", "buy", "market", None, 2.0),
        (20, 25, "S1", "new", "S", "sell", "limit_maker", 101.01, 1.0),
        (60, 65, "S1", "replace", "S", "sell", "limit_maker", 101.02, 1.0),
        (80, 85, "S1", "new", "R", "buy", "limit_maker", 99.0, 1.0),
        (160, 170, "S1", "cancel", "S", None, None, None, None),
        (175, 180, "S1", "cancel", "S", None, None, None, None),
    ])
    reports = _reports([
        (5, "B", "ack", None, "buy", "market", 0.0, 2.0, None, None, 2.0, None, None),
        (5, "B", "fill", None, "buy", "market", 0.0, 2.0, 100.01, 1.0, 1.0, "taker", 0.1),
        (5, "B", "fill", None, "buy", "market", 0.0, 2.0, 100.02, 1.0, 0.0, "taker", 0.1),
        (25, "S", "ack", None, "sell", "limit_maker", 101.01, 1.0, None, None, 1.0, None, None),
        (65, "S", "cancelled", "replaced", "sell", "limit_maker", 101.01, 1.0, None, None, 0.0, None, None),
        (65, "S", "ack", None, "sell", "limit_maker", 101.02, 1.0, None, None, 1.0, None, None),
        (85, "R", "rejected", "max_open_orders", "buy", "limit_maker", 99.0, 1.0, None, None, 0.0, None, None),
        (150, "S", "fill", None, "sell", "limit_maker", 101.02, 1.0, 101.02, 0.4, 0.6, "maker", 0.04),
        (170, "S", "cancelled", "user", "sell", "limit_maker", 101.02, 1.0, None, None, 0.0, None, None),
    ])
    rejects = pl.DataFrame({"exchange_ns": [180], "owner": ["S1"], "request": ["cancel"], "order_id": ["S"],
                            "reason": ["unknown_order"]})
    positions = pl.DataFrame({"ts_ns": [300], "owner": ["S1"], "position": [1.6], "mark": [102.0], "pnl": [0.0]})
    meta = {"window": {"start_ns": 0, "trading_ns": 0, "end_ns": 300}}
    return FakeRun(meta, {"orders": requests, "exec_reports": reports, "request_rejects": rejects,
                          "positions": positions}), market()


def test_hand_computed_orders():
    run, md = hand_case()
    o = tca.orders(run, md)
    assert o.select("order_id", "version", "request", "status", "end_reason").rows() == [
        ("B", 1, "new", "filled", None), ("S", 1, "new", "cancelled", "replaced"),
        ("S", 2, "replace", "cancelled", "user"), ("R", 1, "new", "rejected", "max_open_orders")]
    b, s1, s2, r = o.iter_rows(named=True)
    # B: 2 bought at an average 100.015 against an arrival mid of 100: 0.03 paid, 1.5 bps
    assert b["avg_px"] == pytest.approx(100.015) and b["mid_sent"] == 100.0
    assert b["shortfall"] == pytest.approx(0.03) and b["shortfall_bps"] == pytest.approx(1.5)
    assert (b["time_to_first_fill_ms"], b["time_to_fill_ms"]) == (0.0, 0.0) and b["opportunity_cost"] == 0.0
    assert b["fees"] == pytest.approx(0.2) and b["maker_qty"] == 0.0
    # S, first version: nothing filled, the mid unchanged until the replace: no opportunity cost
    assert s1["filled_qty"] == 0.0 and s1["opportunity_cost"] == 0.0 and s1["shortfall_bps"] is None
    assert (s1["sent_ns"], s1["arrive_ns"], s1["end_ns"]) == (20, 25, 65)
    # S, second version: 0.4 sold at 101.02 against 100 (a gain), 0.6 unsold while the mid rose to 101
    assert s2["sent_ns"] == 60 and s2["end_ns"] == 170 and s2["maker_qty"] == pytest.approx(0.4)
    assert s2["shortfall"] == pytest.approx(-0.408) and s2["shortfall_bps"] == pytest.approx(-102.0)
    assert s2["opportunity_cost"] == pytest.approx(-0.6)
    assert s2["time_to_first_fill_ms"] == pytest.approx(85e-6) and s2["time_to_fill_ms"] is None
    # R: rejected, no unfilled quantity counted
    assert r["opportunity_cost"] == 0.0 and r["fills"] == 0


def test_hand_computed_fill_costs_and_slippage():
    run, md = hand_case()
    f = tca.fill_costs(run, 50, md)
    # Buys at 100.01 / 100.02 against 100, the mid unchanged 50 ns later; the sell at 101.02 against 101, the mid
    # 102 by t=200: realized cost 2 x (102 - 101.02) / 101, impact -2 / 101 (the market rose after it sold)
    assert f["effective_spread_bps"].to_list() == pytest.approx([2.0, 4.0, -0.04 / 101 * 1e4])
    assert f["realized_spread_bps"].to_list() == pytest.approx([2.0, 4.0, 1.96 / 101 * 1e4])
    assert f["price_impact_bps"].to_list() == pytest.approx([0.0, 0.0, -2.0 / 101 * 1e4])
    assert f["effective_spread"].to_list() == pytest.approx([0.02, 0.04, -0.016])

    sl = {row["side"]: row for row in tca.slippage(run, md).iter_rows(named=True)}
    buy, sell = sl["buy"], sl["sell"]
    assert (buy["start_ns"], buy["end_ns"]) == (0, 6) and buy["market_vwap"] == pytest.approx(100.01)
    assert buy["market_volume"] == 3.0 and buy["participation"] == pytest.approx(2 / 3)
    assert buy["vs_vwap_bps"] == pytest.approx(0.005 / 100.01 * 1e4) and buy["vs_twap_bps"] == pytest.approx(1.5)
    # The sell: from its first sent placement with a fill (the replace, t=60) to its fill (t=150)
    twap = (40 * 100 + 51 * 101) / 91
    assert (sell["start_ns"], sell["end_ns"]) == (60, 151) and sell["market_vwap"] == pytest.approx(101.0)
    assert sell["mid_twap"] == pytest.approx(twap)
    assert sell["vs_vwap_bps"] == pytest.approx(-0.02 / 101 * 1e4)
    assert sell["vs_twap_bps"] == pytest.approx(-(101.02 - twap) / twap * 1e4)


def test_hand_computed_summary_and_rejects():
    run, md = hand_case()
    assert tca.rejects(run).rows() == [("S1", "order", "max_open_orders", 1), ("S1", "cancel", "unknown_order", 1)]
    (s,) = tca.summary(run, 50, md).to_dicts()
    assert (s["placements"], s["new"], s["replace"], s["cancel"], s["amend"]) == (4, 3, 1, 2, 0)
    assert (s["rejected"], s["refused_requests"]) == (1, 1)
    assert s["sent_qty"] == 4.0 and s["filled_qty"] == pytest.approx(2.4) and s["fill_ratio"] == pytest.approx(0.6)
    assert s["placements_filled"] == pytest.approx(2 / 3)
    assert s["fills"] == 3 and (s["cancels_user"], s["cancels_replace"]) == (1, 1)
    assert s["cancel_fill_ratio"] == pytest.approx(2 / 3) and s["order_to_trade"] == pytest.approx(6 / 3)
    notional = 2 * 100.015 + 0.4 * 101.02
    assert s["shortfall_bps"] == pytest.approx((0.03 - 0.408) / notional * 1e4)
    assert s["opportunity_cost"] == pytest.approx(-0.6)
    fill_notional = 100 + 100 + 0.4 * 101
    assert s["effective_spread_bps"] == pytest.approx((0.02 + 0.04 - 0.016) / fill_notional * 1e4)


def test_market_vwap_and_twap():
    md = market()
    assert md.vwap(0, 300) == pytest.approx(((100.01 * 3 + 101.0 * 2 + 103.0) / 6, 6.0))
    vwap, vol = md.vwap(3, 100)
    assert np.isnan(vwap) and vol == 0.0
    assert md.twap(0, 200) == pytest.approx((100 * 100 + 80 * 101 + 20 * 102) / 200)
    assert md.twap(150, 150) == 101.0
    t = md.trades()
    assert t["buy"].tolist() == [True, False, True] and t["qty"].tolist() == [3.0, 2.0, 1.0]


# ---- against the simulator ----

def run_fixture(tmp_path, strategy, **extra):
    return Results.run({"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps", "strategy": strategy,
                        "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}, **extra})


QUOTER = {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 500}}
TWAP = {"name": "twap_taker", "params": {"side": "sell", "quantity": "0.05", "slices": 12, "duration_ms": 50000}}


def test_data_trades_are_the_files_trades(tmp_path):
    res = run_fixture(tmp_path, QUOTER)
    t = bs.data_trades(str(FIXTURE))
    assert len(t) == res.meta["synthesizer"]["trades"]
    assert np.all(np.diff(t["ts_ns"].astype(np.int64)) >= 0) and set(np.unique(t["side"])) <= {0, 1}


def test_quoter_placements_match_the_reports(tmp_path):
    res = run_fixture(tmp_path, QUOTER)
    o = tca.orders(res)
    rep = table(res, "exec_reports")
    req = table(res, "orders")
    fills = rep.filter(pl.col("type") == "fill")
    # Every NEW and REPLACE that reached the exchange is a placement; every fill belongs to one
    assert o.height == req.filter(pl.col("request").is_in(["new", "replace"])).height
    assert o["fills"].sum() == fills.height
    assert o["filled_qty"].sum() == pytest.approx(fills["last_qty"].sum())
    assert o.filter(pl.col("status") == "cancelled")["end_reason"].to_list() == ["replaced"] * int(
        (rep["reason"] == "replaced").sum())
    assert o["request"].null_count() == 0 and (o["sent_ns"] <= o["arrive_ns"]).all()
    filled = o.filter(pl.col("status") == "filled")
    assert ((filled["filled_qty"] - filled["quantity"]).abs() < 1e-12).all()
    assert (o["time_to_first_fill_ms"].drop_nulls() >= 0).all()

    (s,) = tca.summary(res).to_dicts()
    assert s["fills"] == fills.height and s["cancels_replace"] == s["replace"]
    assert 0 < s["fill_ratio"] <= 1 and s["rejected"] == 0
    f = tca.fill_costs(res, "1s")
    assert (f["effective_spread_bps"] - f["realized_spread_bps"] - f["price_impact_bps"]).abs().max() < 1e-9


def test_twap_taker_costs(tmp_path):
    res = run_fixture(tmp_path, TWAP)
    (s,) = tca.summary(res).to_dicts()
    assert s["fill_ratio"] == pytest.approx(1.0) and s["placements_filled"] == 1.0
    assert s["cancel_fill_ratio"] == 0.0 and s["order_to_trade"] == pytest.approx(1.0)
    f = tca.fill_costs(res)
    assert (f["liquidity"] == "taker").all() and (f["effective_spread_bps"] >= -1e-9).all()  # sold at or below the mid
    (sl,) = tca.slippage(res).to_dicts()
    assert sl["side"] == "sell" and sl["filled_qty"] == pytest.approx(0.05) and 0 < sl["participation"] < 1


def test_rejects_by_reason(tmp_path):
    res = run_fixture(tmp_path, QUOTER, limits={"open_orders": 1})
    r = tca.rejects(res)
    assert r.select("kind", "reason").rows() == [("order", "max_open_orders")]
    (s,) = tca.summary(res).to_dicts()
    assert s["rejected"] == r["count"][0] == int((table(res, "exec_reports")["type"] == "rejected").sum())
    o = tca.orders(res)
    assert (o.filter(pl.col("status") == "rejected")["filled_qty"] == 0).all()
