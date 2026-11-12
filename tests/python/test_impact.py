"""Impact overlay (P6.1) end to end: a taker's cost rises with its size and with the impact half-life (the Phase 6
sanity check), and half-life 0 is the P3 behaviour."""

from pathlib import Path

import pytest

import blocksprint_sim as bs  # noqa: F401  built by uv sync; a missing module is a failure, not a skip
from research.analytics.pnl import decompose
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"


def twap(tmp_path, quantity, half_life_ms):
    """A TWAP buy over the 1-minute slice: 50 MARKET slices, one a second"""
    return Results.run({
        "dataset": str(FIXTURE), "latency_preset": "zero", "impact": {"half_life_ms": half_life_ms},
        "strategy": {"name": "twap_taker",
                     "params": {"side": "buy", "quantity": quantity, "slices": 50, "duration_ms": 50000}},
        "output": {"dir": str(tmp_path / f"q{quantity}_t{half_life_ms}"), "sample_ms": 1000}})


@pytest.fixture(scope="module")
def grid(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("impact")
    out = {}
    for q in ("0.5", "5", "25"):
        for tau in (0, 1000, 10000):
            res = twap(tmp, q, tau)
            (d,) = decompose(res).to_dicts()
            out[q, tau] = (res, -d["spread_taker"] / d["base_volume"])  # cost per unit over the mid
    return out


def test_cost_rises_with_size_and_half_life(grid):
    sizes, taus = ("0.5", "5", "25"), (0, 1000, 10000)
    for q in sizes:
        costs = [grid[q, t][1] for t in taus]
        assert costs == sorted(costs), (q, costs)
    for t in taus:
        costs = [grid[q, t][1] for q in sizes]
        assert costs == sorted(costs), (t, costs)
    # and the overlay does something at the large size
    assert grid["25", 10000][1] > grid["25", 0][1]


def test_half_life_is_recorded_and_zero_takes_nothing(grid):
    res0, _ = grid["5", 0]
    res1, _ = grid["5", 10000]
    assert res0.meta["simulator"]["impact_half_life_ms"] == 0
    assert res1.meta["simulator"]["impact_half_life_ms"] == 10000
    assert float(res0.meta["synthesizer"]["impact_taken_qty"]) == 0
    assert float(res1.meta["synthesizer"]["impact_taken_qty"]) > 0


def test_negative_half_life_is_refused(tmp_path):
    with pytest.raises(Exception, match="impact.half_life_ms"):
        twap(tmp_path, "1", -1)
