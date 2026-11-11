"""Risk (P5.8): a hand-computed case, and the simulator's own accounts on the committed slice."""

from pathlib import Path

import numpy as np
import polars as pl
import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.analytics import risk
from research.analytics.market import MarketData, table
from research.analytics.risk import YEAR_NS
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


def hand_case():
    """The mid stays at 100. Buy 2 at 100 (t=10), sell 3 at 101 (t=50), buy 1 at 100.5 (t=80); the run ends at
    t=100. Position: 0, then 2 for 40 ns, -1 for 30 ns, 0 for the last 20 ns."""
    bbo = np.zeros(1, dtype=bs.data_bbo(str(FIXTURE), 0, 1).dtype)
    bbo[0] = (0, round(99.99 * E8), E8, round(100.01 * E8), E8)
    market = MarketData(bbo, np.zeros(0, dtype=bs.data_trades(str(FIXTURE), 0, 1).dtype))
    reports = pl.DataFrame({
        "exchange_ns": [10, 50, 60, 80], "owner": ["S1"] * 4, "order_id": ["A", "B", "C", "D"],
        "type": ["fill", "fill", "rejected", "fill"], "reason": [None, None, "max_open_orders", None],
        "side": ["buy", "sell", "buy", "buy"], "liquidity": ["maker", "maker", None, "taker"],
        "last_px": [100.0, 101.0, None, 100.5], "last_qty": [2.0, 3.0, None, 1.0], "fee": [0.0, 0.0, None, 0.0]})
    positions = pl.DataFrame({"ts_ns": [0, 20, 40, 60, 80, 100], "owner": ["S1"] * 6,
                              "pnl": [0.0, 5.0, 3.0, 6.0, 1.0, 4.0], "open_orders": [1, 2, 2, 1, 0, 0]})
    rejects = pl.DataFrame(schema={"owner": pl.String, "request": pl.String, "reason": pl.String})
    meta = {"window": {"start_ns": 0, "trading_ns": 0, "end_ns": 100},
            "config": {"strategy": {"params": {"max_position": "2"}}, "limits": {"open_orders": 2}}}
    return FakeRun(meta, {"exec_reports": reports, "positions": positions, "request_rejects": rejects}), market


def test_hand_computed_ratios():
    run, md = hand_case()
    (r,) = risk.ratios(run, 25, md).to_dicts()
    # Bucket PnL: 0, 0, +3 (sold 3 a unit above the mid), -0.5 (bought half a unit above it)
    rets = np.array([0.0, 0.0, 3.0, -0.5])
    assert r["buckets"] == 4 and r["mean"] == pytest.approx(0.625)
    assert r["std"] == pytest.approx(rets.std(ddof=1))
    assert r["sharpe"] == pytest.approx(0.625 / rets.std(ddof=1) * np.sqrt(YEAR_NS / 25))
    assert r["sortino"] == pytest.approx(0.625 / 0.25 * np.sqrt(YEAR_NS / 25))  # downside: sqrt(0.25 / 4)
    assert (r["hit_rate"], r["best"], r["worst"]) == (0.25, 3.0, -0.5)
    # A last, shorter bucket is left out
    assert risk.ratios(run, 30, md)["buckets"][0] == 3


def test_hand_computed_drawdown():
    run, _ = hand_case()
    (d,) = risk.drawdown(run).to_dicts()
    # PnL 0, 5, 3, 6, 1, 4: the high of 6 at t=60 falls to 1 at t=80 and isn't regained
    assert d["max_drawdown"] == 5.0 and (d["peak_ns"], d["peak_pnl"]) == (60, 6.0)
    assert (d["trough_ns"], d["trough_pnl"]) == (80, 1.0) and d["recovered_ns"] is None
    assert d["duration_s"] == pytest.approx(40e-9) and d["pnl"] == 4.0
    s = risk.drawdown_series(run)
    assert s["drawdown"].to_list() == [0.0, 0.0, 2.0, 0.0, 5.0, 2.0]


def test_hand_computed_inventory_limits_turnover():
    run, md = hand_case()
    (i,) = risk.inventory(run, md).to_dicts()
    assert i["mean"] == pytest.approx(0.5) and i["mean_abs"] == pytest.approx(1.1)
    assert i["std"] == pytest.approx(np.sqrt(1.65))
    assert (i["min"], i["max"], i["final"]) == (-1.0, 2.0, 0.0)
    assert (i["p05"], i["p50"], i["p95"]) == (-1.0, 0.0, 2.0)
    assert (i["flat"], i["long"], i["short"]) == pytest.approx((0.3, 0.4, 0.3))
    assert i["mean_abs_notional"] == pytest.approx(110.0)

    (lim,) = risk.limits(run, market=md).to_dicts()  # the limit from the strategy's max_position
    assert lim["position_limit"] == 2.0 and lim["time_at_position_limit"] == pytest.approx(0.4)
    assert lim["max_abs_position"] == 2.0
    assert lim["open_orders_limit"] == 2 and lim["samples_at_open_orders_limit"] == pytest.approx(2 / 6)
    assert (lim["rejects_open_orders"], lim["rejects_rate_limit"]) == (1, 0)
    assert risk.limits(run, position_limit=1.0, market=md)["time_at_position_limit"][0] == pytest.approx(0.7)

    (t,) = risk.turnover(run, md).to_dicts()
    assert t["base_volume"] == 6.0 and t["quote_volume"] == pytest.approx(603.5)
    assert t["inventory_turnover"] == pytest.approx(6 / 1.1)
    assert t["holding_time_s"] == pytest.approx(1.1 / (3 / 100e-9))


def test_risk_on_the_slice(tmp_path):
    res = Results.run({"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps",
                       "strategy": {"name": "passive_quoter",
                                    "params": {"size": "0.001", "refresh_ms": 500, "max_position": "0.003"}},
                       "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}})
    (s,) = risk.summary(res, "5s").to_dicts()
    account = res.meta["sessions"][0]["exchange_account"]
    assert s["pnl"] == pytest.approx(float(account["pnl"]))
    assert s["inv_final"] == pytest.approx(float(account["position"]))
    assert s["base_volume"] == pytest.approx(float(account["base_volume"]))
    assert s["position_limit"] == 0.003 and 0 <= s["time_at_position_limit"] <= 1
    assert s["inv_max"] <= 0.003 + 1e-12 and s["inv_min"] >= -0.003 - 1e-12  # the quoter keeps its limit
    assert s["inv_flat"] + s["inv_long"] + s["inv_short"] == pytest.approx(1.0)
    assert s["buckets"] == 11  # 59.98 s of data: eleven whole 5 s buckets
    # The drawdown is the largest fall of the engine's PnL from its running high
    pnl = table(res, "positions")["pnl"].to_numpy()
    assert s["max_drawdown"] == pytest.approx(float(np.max(np.maximum.accumulate(pnl) - pnl)))
