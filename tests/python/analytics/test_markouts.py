"""Markouts (P5.6): a hand-computed case, and consistency with the PnL split and TCA on the committed slice."""

from pathlib import Path

import numpy as np
import polars as pl
import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.analytics import tca
from research.analytics.markouts import fill_markouts, markout_curve
from research.analytics.market import MarketData
from research.analytics.pnl import fills
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
E8 = 100_000_000
MS = 1_000_000


class FakeRun:
    def __init__(self, meta, frames):
        self.meta = meta
        self._frames = frames

    def frame(self, name):
        return self._frames[name]


def hand_case():
    # The mid: 100 from t=0, 101 from 150 ms, 99 from 1.2 s; the data ends at 2 s
    bbo = np.zeros(3, dtype=bs.data_bbo(str(FIXTURE), 0, 1).dtype)
    for i, (ts, bid, ask) in enumerate([(0, 99.99, 100.01), (150 * MS, 100.99, 101.01), (1200 * MS, 98.99, 99.01)]):
        bbo[i] = (ts, round(bid * E8), E8, round(ask * E8), E8)
    market = MarketData(bbo, end_ns=2000 * MS)
    reports = pl.DataFrame({
        "exchange_ns": [100 * MS, 100 * MS, 1500 * MS], "owner": ["S1"] * 3, "order_id": ["B", "S", "T"],
        "type": ["fill"] * 3, "side": ["buy", "sell", "buy"], "liquidity": ["maker", "maker", "taker"],
        "last_px": [99.99, 100.01, 99.01], "last_qty": [2.0, 1.0, 1.0], "fee": [0.0, 0.0, 0.1]})
    return FakeRun({"window": {"start_ns": 0, "trading_ns": 0, "end_ns": 2000 * MS}},
                   {"exec_reports": reports}), market


def test_hand_computed_markouts():
    run, md = hand_case()
    f = fill_markouts(run, ["0ms", "100ms", "1s"], md)
    # B buys 2 at 99.99 (mid 100): +0.01 at once; at 200 ms the mid is 101: +1.01; at 1.1 s still 101
    # S sells 1 at 100.01: +0.01; then -0.99, -0.99
    # T buys 1 at 99.01 at 1.5 s (mid 99): -0.01; 1.6 s: -0.01; 2.5 s is past the data's end: null
    assert f["mo_0ms"].to_list() == pytest.approx([0.02, 0.01, -0.01])
    assert f["mo_100ms"].to_list() == pytest.approx([2.02, -0.99, -0.01])
    assert f["mo_1s"].to_list()[:2] == pytest.approx([2.02, -0.99]) and f["mo_1s"][2] is None
    assert f["mo_100ms_bps"].to_list() == pytest.approx([101.0, -99.0, -1.0 / 99 * 1e4 * 0.01])
    assert f["mo_1s_bps"][2] is None

    c = markout_curve(run, ["0ms", "100ms", "1s"], by=("liquidity",), market=md)
    maker = c.filter(pl.col("liquidity") == "maker")
    assert maker["horizon"].to_list() == ["0ms", "100ms", "1s"]
    # Quantity-weighted: (2 x 101 + 1 x -99) / 3 bps at 100 ms
    assert maker["mean_bps"].to_list() == pytest.approx([1.0, 103 / 3, 103 / 3])
    assert maker["pnl"].to_list() == pytest.approx([0.03, 1.03, 1.03])
    assert maker["fills"].to_list() == [2, 2, 2] and maker["qty"].to_list() == [3.0, 3.0, 3.0]
    taker = c.filter(pl.col("liquidity") == "taker")
    assert taker["fills"].to_list() == [1, 1]  # nothing past the data's end
    assert taker["horizon"].to_list() == ["0ms", "100ms"]


def test_horizon_labels_and_defaults():
    run, md = hand_case()
    f = fill_markouts(run, market=md)
    assert [c for c in f.columns if c.startswith("mo_") and c.endswith("_bps")] == [
        "mo_0ms_bps", "mo_100ms_bps", "mo_1s_bps", "mo_5s_bps", "mo_30s_bps", "mo_60s_bps"]
    assert "mo_250us" in fill_markouts(run, [250_000], md).columns
    with pytest.raises(ValueError):
        fill_markouts(run, ["-1s"], md)


def test_markouts_agree_with_pnl_and_tca(tmp_path):
    res = Results.run({"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps",
                       "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 500}},
                       "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}})
    f = fill_markouts(res, ["0ms", "1s"])
    # At h = 0 the markout is the spread capture; the move after it is half TCA's price impact
    assert f["mo_0ms"].to_list() == pytest.approx(fills(res)["spread_capture"].to_list())
    impact = tca.fill_costs(res, "1s")["price_impact_bps"].to_numpy()
    moved = (f["mo_1s_bps"] - f["mo_0ms_bps"]).to_numpy()
    ok = ~np.isnan(moved.astype(float))
    assert np.allclose(moved[ok], impact[ok] / 2)
    # The fixture is one minute: fills in its last second have no 1 s markout
    end = res.meta["data_header"]["last_ts_local"]
    late = f["exchange_ns"] + 1_000_000_000 > end
    assert f.filter(late)["mo_1s"].null_count() == late.sum()
    assert f.filter(~late)["mo_1s"].null_count() == 0

    c = markout_curve(res, ["0ms", "1s"], by=())
    assert c["fills"][0] == f.height and c["pnl"][0] == pytest.approx(f["mo_0ms"].sum())
    assert c["fills"][1] == f.height - late.sum()
    sides = markout_curve(res, ["0ms"], by=("side",))
    assert sorted(sides["side"].to_list()) == ["buy", "sell"] and sides["fills"].sum() == f.height
