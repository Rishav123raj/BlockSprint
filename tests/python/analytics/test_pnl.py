"""PnL decomposition (P5.4): a hand-computed case, and the simulator's own accounts on the committed slice."""

from pathlib import Path

import numpy as np
import polars as pl
import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.analytics.market import MarketData, table
from research.analytics.pnl import decompose, fills, parse_every, pnl_buckets
from research.experiments.runner import run_experiment
from research.experiments.schema import Experiment
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
E8 = 100_000_000


def quoter_config(tmp_path, **extra):
    return {"dataset": str(FIXTURE), "latency_preset": "zero",
            "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 500}},
            "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}, **extra}


def twap_config(tmp_path):
    return {"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps",
            "strategy": {"name": "twap_taker",
                         "params": {"side": "sell", "quantity": "0.05", "slices": 12, "duration_ms": 50000}},
            "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}}


# ---- a hand-computed case ----

class FakeRun:
    """meta + frame(): what the analytics read from a run"""

    def __init__(self, meta, frames):
        self.meta = meta
        self._frames = frames

    def frame(self, name):
        return self._frames[name]


def bbo(rows):
    """(ts, bid, ask) in quote units -> a data_bbo-shaped array"""
    a = np.zeros(len(rows), dtype=bs.data_bbo(str(FIXTURE), 0, 1).dtype)
    for i, (ts, bid, ask) in enumerate(rows):
        a[i] = (ts, round(bid * E8), E8, round(ask * E8), E8)
    return a


def hand_case():
    # The mid: 100 from t=0, 101 from t=10, 99 from t=20; the run ends at t=30
    market = MarketData(bbo([(0, 99.99, 100.01), (10, 100.99, 101.01), (20, 98.99, 99.01)]))
    reports = pl.DataFrame({
        "exchange_ns": [5, 15], "owner": ["S1", "S1"], "order_id": ["S1-1", "S1-2"], "type": ["fill", "fill"],
        "side": ["buy", "sell"], "liquidity": ["maker", "taker"],
        "last_px": [99.99, 101.01], "last_qty": [2.0, 1.0], "fee": [0.1, 0.05]})
    # cash -2 x 99.99 + 101.01 = -98.97; 1 held at 99; fees 0.15
    positions = pl.DataFrame({"ts_ns": [30], "owner": ["S1"], "position": [1.0], "mark": [99.0],
                              "pnl": [-98.97 + 99.0 - 0.15]})
    meta = {"window": {"start_ns": 0, "trading_ns": 0, "end_ns": 30}}
    return FakeRun(meta, {"exec_reports": reports, "positions": positions}), market


def test_hand_computed_decomposition():
    run, market = hand_case()
    f = fills(run, market)
    assert f["mid"].to_list() == [100.0, 101.0]
    # buy 2 at 99.99 against 100: +0.02; sell 1 at 101.01 against 101: +0.01
    assert f["spread_capture"].to_list() == pytest.approx([0.02, 0.01])
    assert f["edge_bps"].to_list() == pytest.approx([1.0, 0.01 / 101 * 1e4])

    (d,) = decompose(run, market).to_dicts()
    # inventory: 2 held from 100 to 101 (+2), then 1 held from 101 to 99 (-2)
    assert d["spread_capture"] == pytest.approx(0.03)
    assert (d["spread_maker"], d["spread_taker"]) == pytest.approx((0.02, 0.01))
    assert d["inventory"] == pytest.approx(0.0, abs=1e-12)
    assert d["fees"] == pytest.approx(0.15)
    assert d["pnl"] == pytest.approx(-0.12) and d["residual"] == pytest.approx(0.0, abs=1e-12)
    assert (d["fills"], d["maker_fills"], d["taker_fills"]) == (2, 1, 1)
    assert d["quote_volume"] == pytest.approx(2 * 99.99 + 101.01)

    b = pnl_buckets(run, 10, market)
    assert b["start_ns"].to_list() == [0, 10, 20] and b["end_ns"].to_list() == [10, 20, 30]
    # A bucket edge sees the book before the batch at that time: the move at t=10 falls in [10, 20)
    assert b["spread_capture"].to_list() == pytest.approx([0.02, 0.01, 0.0])
    assert b["inventory"].to_list() == pytest.approx([0.0, 2.0, -2.0])
    assert b["fees"].to_list() == pytest.approx([0.1, 0.05, 0.0])
    assert b["pnl"].to_list() == pytest.approx([-0.08, 1.96, -2.0])
    assert b["position"].to_list() == pytest.approx([2.0, 1.0, 1.0]) and b["mid"].to_list() == [100.0, 101.0, 99.0]
    assert b["cum_pnl"][-1] == pytest.approx(d["pnl"])

    # Uneven buckets: the last one is shorter
    assert pnl_buckets(run, 12, market)["end_ns"].to_list() == [12, 24, 30]


def test_market_lookup_sides_and_invalid_book():
    md = MarketData(bbo([(0, 0, 0), (10, 99.0, 101.0), (20, 0, 0), (30, 101.0, 103.0)]))
    assert np.isnan(md.mid([5])).all()
    assert md.mid([10], "right").tolist() == [100.0] and np.isnan(md.mid([10], "left")).all()
    assert md.mid([25]).tolist() == [100.0]  # carried through the gap
    at = md.bbo_at([25, 30])
    assert np.isnan(at["bid"][0]) and at["bid"][1] == 101.0 and at["ask_qty"][1] == 1.0


def test_parse_every():
    assert parse_every("500ms") == 500_000_000 and parse_every("1m") == 60_000_000_000
    assert parse_every(7) == 7 and parse_every("1.5s") == 1_500_000_000
    for bad in ("", "5x", 0, "-1s"):
        with pytest.raises(ValueError):
            parse_every(bad)


# ---- against the simulator ----

def test_timeline_mid_is_the_simulators_mark(tmp_path):
    res = Results.run(quoter_config(tmp_path))
    pos = res.positions
    md = MarketData.for_run(res)
    # The samples see the book before batches at their own time ("left"), in exact 1e-8 units. The first is
    # at the window's start, before the first snapshot: no mid (the simulator writes 0)
    mid = md.mid(pos["ts_ns"], "left")
    assert np.isnan(mid[0]) and pos["mark"][0] == 0
    assert np.array_equal(np.round(mid[1:] * E8).astype(np.int64), pos["mark"][1:])


@pytest.mark.parametrize("make", [quoter_config, twap_config], ids=["quoter", "twap"])
def test_decomposition_adds_up_to_the_engines_pnl(tmp_path, make):
    res = Results.run(make(tmp_path))
    (d,) = decompose(res).to_dicts()
    final = res.meta["sessions"][0]["exchange_account"]
    assert d["pnl"] == pytest.approx(float(final["pnl"]))
    assert abs(d["residual"]) < 1e-6
    assert d["spread_capture"] == pytest.approx(d["spread_maker"] + d["spread_taker"])
    assert d["fees"] == pytest.approx(float(final["fees"]))
    assert d["fills"] == int(final["fills"])

    # The buckets' cumulative pnl is positions.csv's pnl at every sample
    b = pnl_buckets(res, "1s", start="window")
    pos = table(res, "positions").select(pl.col("ts_ns").alias("end_ns"), pl.col("pnl").alias("engine"))
    j = b.join(pos, on="end_ns")
    assert j.height >= b.height - 1
    assert (j["cum_pnl"] - j["engine"]).abs().max() < 1e-6
    assert b["pnl"].sum() == pytest.approx(d["pnl"], abs=1e-6)


def test_twap_taker_pays_the_spread(tmp_path):
    res = Results.run(twap_config(tmp_path))
    (d,) = decompose(res).to_dicts()
    assert d["taker_fills"] > 0 and d["maker_fills"] == 0
    assert d["spread_taker"] < 0 and d["edge_bps"] < 0  # selling at or below the mid
    f = fills(res)
    assert (f["edge_bps"] <= 1e-9).all()


def test_stored_run_gives_the_same_numbers(tmp_path):
    exp = Experiment.model_validate({
        "name": "pnl", "dataset": str(FIXTURE), "latency_preset": "zero", "results": str(tmp_path / "results"),
        "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 500}}})
    result = run_experiment(exp, workers=1, progress=None)
    stored = result.load(result.runs[0])
    in_memory = Results.run(quoter_config(tmp_path))
    a, b = decompose(stored), decompose(in_memory)
    for col in ("pnl", "spread_capture", "inventory", "fees", "residual"):
        assert a[col][0] == pytest.approx(b[col][0], abs=1e-9)
    assert pnl_buckets(stored, "10s")["pnl"].to_list() == pytest.approx(pnl_buckets(in_memory, "10s")["pnl"].to_list())
