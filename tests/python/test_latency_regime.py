"""Regime latency (P6.3) end to end: a flat curve at 1 is the run without a regime, byte for byte; a curve
above 1 slows every delay and is recorded in run_meta.json."""

from pathlib import Path

import pytest

import blocksprint_sim as bs  # noqa: F401  built by uv sync; a missing module is a failure, not a skip
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"


def quoter(tmp_path, name, regime=None):
    cfg = {"dataset": str(FIXTURE), "latency_preset": "binance_spot_home_pc",
           "strategy": {"name": "passive_quoter", "params": {"refresh_ms": 100}},
           "output": {"dir": str(tmp_path / name), "sample_ms": 1000}}
    if regime is not None:
        cfg["latency_regime"] = regime
    return Results.run(cfg)


def test_flat_curve_at_one_changes_nothing(tmp_path):
    off = quoter(tmp_path, "off")
    flat = quoter(tmp_path, "flat", {"curve": [[0, 1]]})
    assert off.meta["simulator"]["latency_regime"] is None
    assert flat.meta["simulator"]["latency_regime"]["max_multiplier"] == 1
    assert flat.output_hash == off.output_hash


def test_a_busy_market_slows_every_channel(tmp_path):
    off = quoter(tmp_path, "off")
    slow = quoter(tmp_path, "slow", {"curve": [[0, 4], [100000, 4]]})
    lr = slow.meta["simulator"]["latency_regime"]
    assert lr["channels"] == ["feed", "entry", "ack"]
    assert lr["mean_multiplier"] == 4 and lr["delays_scaled"] > 0
    assert slow.output_hash != off.output_hash


def test_bad_regime_is_refused(tmp_path):
    with pytest.raises(Exception, match="latency_regime.curve"):
        quoter(tmp_path, "bad", {"curve": [[10, 1], [5, 2]]})
