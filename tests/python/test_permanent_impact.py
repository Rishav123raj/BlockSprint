"""Permanent impact (P6.6) end to end: a TWAP's cost over the data mid rises with its size and the coefficient,
the market ends moved by the square-root law, and the default is off."""

import math
from pathlib import Path

import pytest

import blocksprint_sim as bs  # noqa: F401  built by uv sync; a missing module is a failure, not a skip
from research.analytics.pnl import decompose
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
SIGMA, VOLUME = 0.025, 20000  # BTC: 2.5% a day, 20,000 BTC a day


def twap(tmp_path, quantity, coefficient, side="buy"):
    """A TWAP over the 1-minute slice: 50 MARKET slices, one a second"""
    impact = {"permanent_coefficient": coefficient, "daily_volatility": SIGMA, "daily_volume": str(VOLUME)}
    return Results.run({
        "dataset": str(FIXTURE), "latency_preset": "zero", "impact": impact,
        "strategy": {"name": "twap_taker",
                     "params": {"side": side, "quantity": quantity, "slices": 50, "duration_ms": 50000}},
        "output": {"dir": str(tmp_path / f"{side}{quantity}_y{coefficient}"), "sample_ms": 1000}})


def cost(res):
    (d,) = decompose(res).to_dicts()
    return -d["spread_taker"] / d["base_volume"]  # per unit, over the data mid


@pytest.fixture(scope="module")
def grid(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("permanent")
    return {(q, y): twap(tmp, q, y) for q in ("0.5", "5", "25") for y in (0, 0.5, 1)}


def test_cost_rises_with_size_and_coefficient(grid):
    sizes, ys = ("0.5", "5", "25"), (0, 0.5, 1)
    for q in sizes:
        costs = [cost(grid[q, y]) for y in ys]
        assert costs == sorted(costs) and costs[-1] > costs[0] + 1, (q, costs)
    for y in ys:
        costs = [cost(grid[q, y]) for q in sizes]
        assert costs == sorted(costs), (y, costs)


def test_the_market_ends_moved_by_the_square_root_law(grid):
    for q in ("0.5", "5", "25"):
        res = grid[q, 1]
        pi = res.meta["simulator"]["permanent_impact"]
        assert float(pi["net_taken_qty"]) == pytest.approx(float(q))
        assert pi["shifts"] > 0
        mark = float(res.meta["final_mark"])
        # shift = Y sigma sqrt(X / V) at the last take's price, in whole 0.01 ticks
        assert float(pi["final_shift"]) == pytest.approx(SIGMA * math.sqrt(float(q) / VOLUME) * mark, abs=0.5)
    # the shift is linear in the coefficient
    assert float(grid["25", 0.5].meta["simulator"]["permanent_impact"]["final_shift"]) == pytest.approx(
        float(grid["25", 1].meta["simulator"]["permanent_impact"]["final_shift"]) / 2, abs=0.02)


def test_off_by_default_and_a_seller_moves_it_down(grid, tmp_path):
    assert grid["5", 0].meta["simulator"]["permanent_impact"] is None
    assert grid["5", 0].meta["config"]["impact"]["permanent_coefficient"] == 0
    sell = twap(tmp_path, "5", 1, side="sell")
    assert float(sell.meta["simulator"]["permanent_impact"]["final_shift"]) < 0
    assert cost(sell) > cost(grid["5", 0])


def test_a_coefficient_needs_volatility_and_volume(tmp_path):
    with pytest.raises(Exception, match="impact.daily_volatility"):
        Results.run({"dataset": str(FIXTURE), "latency_preset": "zero", "impact": {"permanent_coefficient": 1},
                     "strategy": {"name": "twap_taker", "params": {"side": "buy", "quantity": "1"}},
                     "output": {"dir": str(tmp_path / "bad")}})
