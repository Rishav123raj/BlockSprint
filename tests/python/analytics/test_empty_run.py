"""A run that sends no orders (P5.13): every analytic gives zero rows or zeros, never an error, and the tables keep
their columns, so code that selects a column works on any run."""

from pathlib import Path

import pytest

from research.analytics import markouts, pnl, queue, report, risk, tca
from research.analytics.market import MarketData
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"


@pytest.fixture(scope="module")
def idle(tmp_path_factory):
    res = Results.run({"dataset": str(FIXTURE), "latency_preset": "zero", "strategy": {"name": "null"},
                       "output": {"dir": str(tmp_path_factory.mktemp("idle")), "sample_ms": 1000}})
    return res, MarketData.for_run(res)


@pytest.mark.parametrize("fn, column", [
    (pnl.fills, "spread_capture"), (tca.orders, "filled_qty"), (tca.fill_costs, "effective_spread_bps"),
    (tca.slippage, "vs_vwap_bps"), (tca.rejects, "reason"), (markouts.fill_markouts, "mo_1s_bps"),
    (markouts.markout_curve, "mean_bps"), (queue.placements, "ahead_qty"), (queue.fill_curve, "fill_rate"),
    (queue.summary, "fill_rate"),
], ids=lambda x: getattr(x, "__qualname__", x))
def test_tables_are_empty_with_their_columns(idle, fn, column):
    res, market = idle
    t = fn(res) if fn is tca.rejects else fn(res, market=market)
    assert t.height == 0 and column in t.columns


def test_summaries_are_zero(idle):
    res, market = idle
    (d,) = pnl.decompose(res, market).to_dicts()
    assert d["pnl"] == 0.0 and d["fills"] == 0 and d["edge_bps"] == 0.0
    assert pnl.pnl_buckets(res, "10s", market)["pnl"].to_list() == [0.0] * 6
    (t,) = tca.summary(res, market=market).to_dicts()
    assert t["placements"] == 0 and t["fills"] == 0 and t["opportunity_cost"] == 0.0
    (r,) = risk.summary(res, market=market).to_dicts()
    assert r["pnl"] == 0.0 and r["max_drawdown"] == 0.0 and r["inv_flat"] == 1.0 and r["quote_volume"] == 0.0
    assert risk.drawdown_series(res)["drawdown"].to_list() == [0.0] * 61


def test_tearsheet_of_an_idle_run(idle):
    page = report.tearsheet(idle[0])
    assert page.rstrip().endswith("</html>") and "PnL, mark-to-mid" in page
