"""Reference strategies in Python (P5.10): the execution schedules, the Avellaneda-Stoikov quotes, the imbalance
signal, and each strategy's behaviour on the committed slice."""

import html
import math
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import polars as pl
import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.analytics import report
from research.analytics.market import table
from research.experiments.runner import run_experiment
from research.experiments.schema import Experiment
from research.sim.results import Results
from research.strategies.avellaneda_stoikov import AvellanedaStoikov
from research.strategies.execution import Executor
from research.strategies.imbalance_taker import ImbalanceTaker, imbalance

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
E8 = 100_000_000
S = 1_000_000_000


def run(tmp_path, strategy, **extra):
    cfg = {"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps", "strategy": {"name": "null"},
           "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}, **extra}
    return Results.run(cfg, strategy)


def fills_of(res):
    return table(res, "exec_reports").filter(pl.col("type") == "fill")


# ---- parameters ----

@pytest.mark.parametrize("make", [
    lambda: Executor(side="hold"), lambda: Executor(mode="vwap"), lambda: Executor(participation=0),
    lambda: Executor(participation=1.5), lambda: Executor(quantity="0"), lambda: Executor(style="iceberg"),
    lambda: Executor(duration_ms=0), lambda: Executor(max_behind=2),
    lambda: AvellanedaStoikov(gamma=0), lambda: AvellanedaStoikov(k=-1), lambda: AvellanedaStoikov(size="0.02"),
    lambda: AvellanedaStoikov(min_spread_ticks=-1), lambda: AvellanedaStoikov(refresh_ms=0),
    lambda: ImbalanceTaker(levels=0), lambda: ImbalanceTaker(threshold=0), lambda: ImbalanceTaker(threshold=1.2),
    lambda: ImbalanceTaker(hold_ms=-1),
])
def test_bad_parameters_are_refused(make):
    with pytest.raises(ValueError):
        make()


def test_schedules():
    twap = Executor(quantity="1", mode="twap", duration_ms=100_000)
    assert twap.target(0) == 0
    twap.start_ns = 0
    assert twap.target(25 * S) == E8 // 4 and twap.target(100 * S) == E8 and twap.target(500 * S) == E8
    pov = Executor(quantity="1", mode="pov", participation=0.1)
    pov.start_ns, pov.market_volume = 0, 3 * E8
    assert pov.target(0) == 3 * E8 // 10
    pov.market_volume = 50 * E8
    assert pov.target(0) == E8  # never more than the quantity


def test_avellaneda_stoikov_quotes():
    st = AvellanedaStoikov(gamma=0.01, k=50, horizon_s=10, sigma0=3.0)
    risk = 0.01 * 9.0 * 10  # gamma sigma^2 tau
    spread = risk + (2 / 0.01) * math.log(1 + 0.01 / 50)
    bid, ask = st.quotes_for(100.0, 0.0)
    assert (bid, ask) == pytest.approx((100 - spread / 2, 100 + spread / 2))
    # Long 2 units: both quotes move down by 2 x gamma sigma^2 tau
    bid2, ask2 = st.quotes_for(100.0, 2.0)
    assert (bid - bid2, ask - ask2) == pytest.approx((2 * risk, 2 * risk))
    # The variance: an exponentially weighted mean of (change in mid)^2 / dt
    st.update_vol(100.0, 0)
    st.update_vol(102.0, 60 * S)  # one half-life: halfway from 9 to 4 / 60
    assert st.var == pytest.approx(9.0 + 0.5 * (4.0 / 60 - 9.0))


def test_imbalance():
    def book(bids, asks):
        levels = {bs.Side.BUY: [SimpleNamespace(qty=q) for q in bids], bs.Side.SELL: [SimpleNamespace(qty=q) for q in asks]}
        return SimpleNamespace(top=lambda side, n: levels[side][:n])

    assert imbalance(book([3, 5], [1, 9]), 1) == pytest.approx(0.5)
    assert imbalance(book([3, 5], [1, 9]), 2) == pytest.approx((8 - 10) / 18)
    assert imbalance(book([], [1]), 1) is None


# ---- on the slice ----

def test_twap_market_is_done_by_the_deadline(tmp_path):
    st = Executor(side="buy", quantity="0.03", mode="twap", duration_ms=40_000, interval_ms=1000)
    res = run(tmp_path, st)
    f = fills_of(res)
    assert f["last_qty"].sum() == pytest.approx(0.03) and (f["liquidity"] == "taker").all()
    start = st.start_ns  # the schedule starts at the first timer after the warm-up
    assert start == res.meta["window"]["trading_ns"] + S
    assert f["exchange_ns"].max() < start + 40 * S + S // 10  # the last slice goes out at the deadline
    # Evenly: never more than one interval's share (plus a lot) ahead of the straight line
    t = (f["exchange_ns"].to_numpy() - start) / 1e9
    done = np.cumsum(f["last_qty"].to_numpy())
    assert np.all(done <= 0.03 * np.minimum(t / 40, 1) + 0.03 / 40 + 1e-5 + 1e-9)


def test_pov_follows_the_market_volume(tmp_path):
    st = Executor(side="sell", quantity="1", mode="pov", participation=0.05, interval_ms=1000)
    res = run(tmp_path, st)
    filled = float(fills_of(res)["last_qty"].sum())
    trades = bs.data_trades(str(FIXTURE))
    start, end = res.meta["window"]["trading_ns"], res.meta["window"]["end_ns"]
    ts = trades["ts_ns"].astype(np.int64)
    vol = trades["qty"][(ts >= start) & (ts < end)].sum() / E8
    vol_lagged = trades["qty"][(ts >= start) & (ts < end - 2 * S)].sum() / E8  # the last slices may not have gone
    assert 0.05 * vol_lagged - 0.0002 <= filled <= 0.05 * vol + 1e-5
    assert st.market_volume / E8 == pytest.approx(trades["qty"][(ts >= st.start_ns) & (ts < end)].sum() / E8,
                                                  rel=0.02)


def test_passive_twap_rests_then_catches_up(tmp_path):
    st = Executor(side="buy", quantity="0.02", mode="twap", duration_ms=40_000, style="passive", max_behind=0.5)
    res = run(tmp_path, st)
    f = fills_of(res)
    assert f["last_qty"].sum() == pytest.approx(0.02)
    assert (f["liquidity"] == "maker").any()  # the price fell through its resting bids
    req = table(res, "orders")
    assert (req.filter(pl.col("request") == "new")["order_type"].is_in(["limit_maker", "market"])).all()


def test_avellaneda_stoikov_on_the_slice(tmp_path):
    st = AvellanedaStoikov(max_position="0.003", refresh_ms=250)
    res = run(tmp_path, st)
    f = fills_of(res)
    rep = table(res, "exec_reports")
    assert f.height > 0 and (f["liquidity"] == "maker").all()  # post-only quotes never cross
    assert (rep["type"] != "rejected").all()
    pos = table(res, "positions")["position"].abs().max()
    assert pos <= 0.003 + 1e-12
    assert st.quotes > 100 and 0 < st.var < 1e4
    again = run(tmp_path, AvellanedaStoikov(max_position="0.003", refresh_ms=250))
    assert again.output_hash == res.output_hash  # deterministic


def test_imbalance_taker_round_trips(tmp_path):
    st = ImbalanceTaker(threshold=0.5, hold_ms=500, cooldown_ms=500)
    res = run(tmp_path, st)
    f = fills_of(res)
    assert st.entries > 0 and (f["liquidity"] == "taker").all()
    q = np.where(f["side"].to_numpy() == "buy", 1, -1) * f["last_qty"].to_numpy()
    path = np.cumsum(q)
    assert np.all(np.abs(path) <= 0.001 + 1e-12)  # one position of `size` at a time
    final = float(res.meta["sessions"][0]["exchange_account"]["position"])
    assert abs(final) <= 0.001


def test_strategies_run_from_an_experiment(tmp_path):
    exp = Experiment.model_validate({
        "name": "refs", "dataset": str(FIXTURE), "latency_preset": "zero", "results": str(tmp_path / "results"),
        "strategy": {"python": "research.strategies.avellaneda_stoikov:AvellanedaStoikov",
                     "params": {"max_position": "0.003"}},
        "sweep": {"params": {"strategy.params.gamma": [0.0005, 0.005]}}})
    result = run_experiment(exp, workers=1, progress=None)
    assert [r.status for r in result.runs] == ["ran", "ran"] and result.runs[0].run_id != result.runs[1].run_id
    page = (report.write_reports(result, exp, progress=None).parent / "runs" / f"{result.runs[0].run_id}.html")
    text = html.unescape(page.read_text(encoding="utf-8"))
    assert "avellaneda_stoikov on BTCUSDT" in text and '"gamma":0.0005' in text  # the Python strategy's own params
    with pytest.raises(ValueError, match="gamma"):
        Experiment.model_validate({**exp.model_dump(by_alias=True), "sweep": {"params": {
            "strategy.params.gamma": [0]}}}).plan()
