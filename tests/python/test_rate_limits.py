"""Rate limits (P6.2) end to end: a PassiveQuoter that re-quotes every few milliseconds runs into the order-count
and request-weight limits (the Phase 6 acceptance check); the default quoter doesn't."""

from pathlib import Path

import pytest

import blocksprint_sim as bs  # noqa: F401  built by uv sync; a missing module is a failure, not a skip
from research.analytics import risk
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"


def quoter(tmp_path, name, limits=None, **params):
    cfg = {"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps",
           "strategy": {"name": "passive_quoter", "params": params},
           "output": {"dir": str(tmp_path / name), "sample_ms": 1000}}
    if limits:
        cfg["limits"] = limits
    return Results.run(cfg)


def stats(res):
    (s,) = res.meta["sessions"]
    return s["stats"]


def test_default_quoter_stays_within_the_limits(tmp_path):
    res = quoter(tmp_path, "default")
    assert (stats(res)["rate_limited"], stats(res)["weight_limited"]) == (0, 0)
    assert risk.limits(res)["rejects_rate_limit"].item() == 0


def test_naive_requoting_is_rate_limited(tmp_path):
    res = quoter(tmp_path, "naive", refresh_ms=5, requote_always=True)
    s = stats(res)
    assert s["rate_limited"] > 0  # orders per 10 s
    assert s["weight_limited"] > 0  # request weight per minute
    assert res.meta["config"]["limits"]["request_weight_per_min"] == 6000
    # the refusals are in the results: rejected orders and refused replaces
    assert risk.limits(res)["rejects_rate_limit"].item() == s["rate_limited"] + s["weight_limited"]


def test_weight_limit_alone(tmp_path):
    # no order-count limit: only request weight refuses
    res = quoter(tmp_path, "weight", limits={"orders_per_10s": 0, "orders_per_day": 0},
                 refresh_ms=5, requote_always=True)
    s = stats(res)
    assert s["rate_limited"] == 0
    assert s["weight_limited"] > 0


def test_negative_weight_limit_is_refused(tmp_path):
    with pytest.raises(Exception, match="limits.request_weight_per_min"):
        quoter(tmp_path, "bad", limits={"request_weight_per_min": -1})
