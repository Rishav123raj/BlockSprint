"""Gap policies (P6.5) end to end: halt, flatten and continue, on the config path and across a real gap."""

import json
import os
from pathlib import Path

import pytest

import blocksprint_sim as bs  # noqa: F401  built by uv sync; a missing module is a failure, not a skip
import polars as pl

from research.analytics import risk
from research.analytics.market import table
from research.experiments.schema import Experiment
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
NOV10 = ROOT / "data" / "packed" / "binance_spot_btc_eth_1110"


def quoter(dataset, out, policy=None, **extra):
    cfg = {"dataset": str(dataset), "latency_preset": "binance_spot_tokyo_vps",
           "strategy": {"name": "passive_quoter", "params": {"refresh_ms": 100}},
           "output": {"dir": str(out), "sample_ms": 1000}, **extra}
    if policy is not None:
        cfg["gap_policy"] = policy
    return Results.run(cfg)


def stats(res):
    (s,) = res.meta["sessions"]
    return s["stats"]


def test_halt_is_the_default_and_a_gapless_run_never_halts(tmp_path):
    res = quoter(FIXTURE, tmp_path / "default")
    assert res.meta["config"]["gap_policy"] == "halt"
    s = stats(res)
    assert (s["gap_halts"], s["gap_rejects"], s["halted_ms"], s["flatten_orders"]) == (0, 0, 0, 0)
    assert risk.limits(res)["rejects_gap_halt"].item() == 0


def test_an_unknown_policy_is_an_error(tmp_path):
    with pytest.raises(Exception, match="gap_policy"):
        quoter(FIXTURE, tmp_path / "bad", "pause")


def test_experiments_pass_the_policy_through():
    e = Experiment.model_validate({"name": "g", "dataset": str(FIXTURE), "gap_policy": "flatten",
                                   "strategy": {"name": "passive_quoter"}})
    assert e.base_config()["gap_policy"] == "flatten"
    with pytest.raises(ValueError):
        Experiment.model_validate({"name": "g", "dataset": str(FIXTURE), "gap_policy": "pause",
                                   "strategy": {"name": "passive_quoter"}})


@pytest.mark.skipif(not (NOV10 / "BTCUSDT.bse").exists(), reason="needs the recorded Nov 10 data (data/packed)")
@pytest.mark.skipif(os.environ.get("BLOCKSPRINT_SLOW_TESTS") != "1",
                    reason="about 40 s (fast-forwards 14.5 hours of data twice): set BLOCKSPRINT_SLOW_TESTS=1")
def test_a_real_gap_halts_then_flatten_closes_at_the_resync(tmp_path):
    # BTCUSDT 2026-11-10: updates were lost at 14:31:38.89; the resync snapshot came 13.71 s later
    window = {"window": {"from": "2026-11-10T14:31:00Z", "to": "2026-11-10T14:32:30Z"}, "symbol": "BTCUSDT"}
    halt = quoter(NOV10, tmp_path / "halt", "halt", **window)
    flat = quoter(NOV10, tmp_path / "flatten", "flatten", **window)
    for res in (halt, flat):
        s = stats(res)
        assert s["gap_halts"] == 1
        assert s["halted_ms"] == pytest.approx(13712.2, abs=0.1)  # seen after the feed latency, both ends
    assert stats(halt)["flatten_orders"] == 0
    assert stats(flat)["flatten_orders"] == 1
    # The quoter pulls its quotes on a gap itself and quotes nothing until the resync: nothing to refuse
    assert stats(halt)["gap_rejects"] == 0
    # The flatten order: one MARKET order at the resync, filled as a taker, for the whole position
    orders = table(flat, "orders")
    market = orders.filter(pl.col("order_type") == "market")
    assert market.height == 1
    gap_seen = 1794321098891068000  # 14:31:38.891068 UTC (data time; the strategy sees it 0.1 ms later)
    assert market["sent_ns"].item() - gap_seen == pytest.approx(13.712e9, abs=0.01e9)
    fills = table(flat, "exec_reports").filter((pl.col("type") == "fill") & (pl.col("liquidity") == "taker"))
    assert fills["order_id"].to_list() == market["order_id"].to_list()
    assert fills["last_qty"].sum() == pytest.approx(market["quantity"].item())
    # Before the flatten, both runs are the same run
    before = pl.col("exchange_ns") < market["sent_ns"].item()
    a = table(halt, "exec_reports").filter(before)
    b = table(flat, "exec_reports").filter(before)
    assert a.equals(b)
    json.dumps(flat.meta)  # the stats serialise
