"""The parity report (P7.5): its measures on hand-made fills, and a whole session where the "paper" side is a
simulator run itself, which must come out identical to its replay."""

import numpy as np
import pytest
from paper_fixture import make_session

from research.analytics.parity import Side, _data_trade_time, measure, render, session_report, summary


class FlatMids:
    """A mid of 100.00 until 10 s, then 100.05."""

    def at(self, ts, before=False):
        ts = np.asarray(ts)
        return np.where(ts < 10 * 10**9, 100.0, 100.05)


def test_measures_on_hand_made_fills():
    side = Side("x",
                {"ts_ns": np.array([0, 0, 2 * 10**9]), "order_id": np.array(["a", "b", "a"], dtype=object),
                 "qty": np.array([0.002, 0.002, 0.002])},
                {"ts_ns": np.array([1 * 10**9, 9_500_000_000]), "order_id": np.array(["b", "a"], dtype=object),
                 "sign": np.array([1.0, -1.0]), "px": np.array([99.99, 100.01]), "qty": np.array([0.002, 0.001]),
                 "fee": np.array([0.0002, 0.0001]), "maker": np.array([True, False])})
    m = measure(side, FlatMids(), tick=0.01, end_ns=20 * 10**9)
    assert m["fills"] == 2 and m["volume"] == pytest.approx(0.003)
    assert m["fill_ratio"] == pytest.approx(0.003 / 0.006) and m["maker_share"] == 0.5
    assert m["time_to_fill_s"]["median"] == pytest.approx(4.25)    # b after 1 s; a 7.5 s after its replace at 2 s
    assert m["spread_ticks"] == pytest.approx(1.0)                  # both 1 tick better than the mid
    # 1 s later: the buy at 99.99 sees 100.00 (+1 tick); the sell at 100.01 sees 100.05 (-4 ticks); by quantity
    assert m["markout_1s_ticks"] == pytest.approx((0.002 * 1 + 0.001 * -4) / 0.003)
    # bought 0.002 at 99.99, sold 0.001 at 100.01; long 0.001 at the end mid 100.05
    assert m["position"] == pytest.approx(0.001)
    assert m["spread_pnl"] == pytest.approx(0.002 * 0.01 + 0.001 * 0.01)
    assert m["pnl"] == pytest.approx(-0.002 * 99.99 + 0.001 * 100.01 + 0.001 * 100.05 - 0.0003)
    assert m["spread_pnl"] + m["inventory_pnl"] - m["fees"] == pytest.approx(m["pnl"])


def test_a_paper_fill_is_timed_at_the_data_trade_that_made_it():
    trades = {"ts": np.array([100, 200, 300]), "px": np.array([10.0, 10.0, 10.01]), "sell": np.array([True, False, True])}
    assert _data_trade_time(trades, 350, 10.0, 1.0) == 100     # a buy fill meets a seller: the one at 100
    assert _data_trade_time(trades, 350, 10.0, -1.0) == 200    # a sell fill meets a buyer
    assert _data_trade_time(trades, 350, 9.99, 1.0) == 0       # nothing at that price


def test_a_session_whose_paper_side_is_the_simulator_matches_its_replay(tmp_path):
    session = make_session(tmp_path)
    r = session_report(session)
    assert r["paper"]["fills"] == r["sim"][0]["fills"] > 0
    assert r["paper"]["fills_matched_to_data"] == 1.0 and r["paper"]["data_time_lag_ms"] == 0.0
    for k in ("fill_ratio", "spread_ticks", "markout_1s_ticks", "markout_5s_ticks", "pnl", "spread_pnl", "fees"):
        assert r["paper"][k] == pytest.approx(r["sim"][0][k]), k
    assert r["checks"]["fill_ratio_rel"] == {"value": pytest.approx(0.0), "pass": True}
    m = r["checks"]["markout_1s_diff_ticks"]
    assert m["pass"] and m["seed_range"] is None and m["tolerance"] == 1.0   # one seed: the 1-tick floor
    s = summary([r])
    assert s["passed"] == {"fill_ratio_rel": True, "markout_1s_diff_ticks": True} and not s["accepted"]  # 1 < 3
    assert "acceptance not met (needs at least 3 sessions)" in render([r])
