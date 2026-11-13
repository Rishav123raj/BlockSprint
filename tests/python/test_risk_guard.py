"""RiskGuard (P6.4) end to end: the strategy's own limits refuse its requests before they leave, and the
drawdown kill switch stops a quoter."""

from pathlib import Path

import pytest

import blocksprint_sim as bs  # noqa: F401  built by uv sync; a missing module is a failure, not a skip
import polars as pl

from research.analytics import risk, tca
from research.analytics.market import table
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"


def quoter(tmp_path, name, guard=None):
    cfg = {"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps",
           "strategy": {"name": "passive_quoter", "params": {"refresh_ms": 100}},
           "output": {"dir": str(tmp_path / name), "sample_ms": 1000}}
    if guard is not None:
        cfg["risk"] = guard
    return Results.run(cfg)


def stats(res):
    (s,) = res.meta["sessions"]
    return s["stats"]


def test_no_limits_refuse_nothing(tmp_path):
    res = quoter(tmp_path, "off")
    assert stats(res)["risk_rejects"] == 0 and stats(res)["risk_killed_at"] == 0
    assert risk.limits(res)["rejects_risk_guard"].item() == 0


def test_one_open_order_refuses_the_second_side(tmp_path):
    res = quoter(tmp_path, "one", {"max_open_orders": 1})
    s = stats(res)
    assert s["risk_rejects"] > 0
    assert risk.limits(res)["rejects_risk_guard"].item() == s["risk_rejects"]
    reasons = tca.rejects(res)
    assert set(reasons["reason"].to_list()) == {"risk_open_orders"}
    # refused before the latency pipe: the reject comes back at the time it was sent
    rep = table(res, "exec_reports").filter(pl.col("reason") == "risk_open_orders")
    sent = table(res, "orders").join(rep.select("order_id", "exchange_ns", "deliver_ns"), on="order_id")
    assert sent.height == rep.height > 0
    assert (sent["arrive_ns"] == sent["sent_ns"]).all()
    assert (sent["deliver_ns"] == sent["sent_ns"]).all()
    assert (table(res, "positions")["open_orders"] <= 1).all()


def test_drawdown_kill_switch_stops_quoting(tmp_path):
    res = quoter(tmp_path, "kill", {"max_drawdown": "0.0001"})
    s = stats(res)
    assert s["risk_killed_at"] > 0
    assert float(s["max_drawdown"]) >= 0.0001
    # Here the quoter's bid fills (0.001) and the kill trips 2 ms later. From then on every bid it sends is
    # refused (and any offer beyond the one that flattens it); that offer fills and leaves it flat.
    rep = table(res, "exec_reports")
    killed = rep.filter(pl.col("reason") == "risk_killed")
    assert killed.height == s["risk_rejects"] > 0
    assert "buy" in set(killed["side"].to_list())
    fills_after = rep.filter((pl.col("type") == "fill") & (pl.col("exchange_ns") > s["risk_killed_at"]))
    assert set(fills_after["side"].to_list()) == {"sell"}
    assert table(res, "positions")["position"].to_list()[-1] == 0


def test_bad_limit_is_refused(tmp_path):
    with pytest.raises(Exception, match="risk.max_position"):
        quoter(tmp_path, "bad", {"max_position": "-1"})
