"""Results as numpy and polars (P4.5): views without a copy, and DataFrames equal to the CSVs value for value."""

import csv
import gc
from decimal import Decimal
from pathlib import Path

import numpy as np
import polars as pl
import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.sim.results import TABLES, Results

FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
SECOND = 1_000_000_000
QUOTER_HASH = "e4983a252e78434fc0614d479c6e5ef46b05d75758e20f4ab3822b5f380c7728"  # determinism_test.cpp


def quoter_config(tmp_path):
    # tests/sim/determinism_test.cpp, scenario "quoter, random latency, prob_power:2"
    return {
        "dataset": str(FIXTURE),
        "latency": {"feed": {"model": "lognormal", "median_us": 2000, "sigma": 0.5, "min_us": 300},
                    "entry": {"model": "normal", "mean_us": 3000, "sd_us": 1000, "min_us": 500},
                    "ack": {"model": "lognormal", "median_us": 2500, "sigma": 0.4, "min_us": 400}},
        "queue_model": {"model": "prob_power", "n": 2},
        "seed": 7,
        "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 100, "max_position": "0.005"}},
        "output": {"dir": str(tmp_path), "sample_ms": 250},
    }


class EveryRequest:
    """Every kind of request, each second: a new bid, amends with and without a price, a replace, a cancel, and
    a cancel of an order that's gone (refused). Covers every column the CSVs leave empty in some rows."""

    def __init__(self):
        self.step = 0
        self.order = None

    def on_start(self, ctx):
        ctx.set_timer(SECOND)

    def on_timer(self, ctx, timer_id):
        ctx.set_timer(SECOND)
        book = ctx.book()
        if not (ctx.can_trade() and book.valid()):
            return
        bid = book.best_bid().price
        tick = book.tick_size
        step = self.step % 6
        self.step += 1
        if step == 0:
            self.order = ctx.send(bs.Side.BUY, bs.OrderType.LIMIT, bid - 50 * tick, bs.to_fixed("0.002"))
        elif step == 1:
            ctx.amend(self.order, bs.to_fixed("0.001"), bid - 60 * tick)
        elif step == 2:
            ctx.amend(self.order, bs.to_fixed("0.0005"))
        elif step == 3:
            ctx.replace(self.order, bid - 40 * tick, bs.to_fixed("0.001"))
        elif step == 4:
            ctx.cancel(self.order)
        else:
            ctx.cancel(self.order)  # already cancelled: refused
            ctx.send(bs.Side.SELL, bs.OrderType.MARKET, 0, bs.to_fixed("0.001"))  # a taker fill


@pytest.fixture(scope="module")
def quoter(tmp_path_factory):
    out = tmp_path_factory.mktemp("quoter")
    return Results.run(quoter_config(out)), out


@pytest.fixture(scope="module")
def requests(tmp_path_factory):
    out = tmp_path_factory.mktemp("requests")
    cfg = {"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps", "strategy": {"name": "null"},
           "output": {"dir": str(out)}}
    return Results.run(cfg, EveryRequest(), "timer_only"), out


def test_numpy_tables_are_views_of_the_runs_rows(quoter):
    res, _ = quoter
    assert res.output_hash == QUOTER_HASH  # keeping the rows changes nothing
    for t in TABLES:
        a = getattr(res, t)
        assert isinstance(a, np.ndarray) and a.dtype.names
        assert len(a) == res.meta["outputs"][f"{t}.csv"]["rows"]
        if len(a):
            assert not a.flags["OWNDATA"] and type(a.base).__name__ == "PyCapsule"  # no copy
    r = res.exec_reports
    assert r.dtype["price"] == np.int64 and r.dtype["type"] == np.uint8
    fills = r[r["type"] == res.codes["type"].index("fill")]
    account = res.meta["sessions"][0]["exchange_account"]
    assert len(fills) == account["fills"] > 0
    assert bs.from_fixed(int(fills["last_qty"].sum())) == account["base_volume"]
    last = res.positions[-1]
    assert bs.from_fixed(int(last["position"])) == account["position"]
    assert res.owners == ["S1"] and res.order_ids[int(r["order"][0])].startswith("S1-")


def test_arrays_outlive_the_simulator():
    import tempfile

    sim = bs.Simulator(bs.SimConfig(quoter_config(Path(tempfile.mkdtemp()))))
    sim.run()
    reports = sim.tables()["exec_reports"]
    expected = reports.copy()
    del sim
    gc.collect()
    assert np.array_equal(reports, expected)


def test_tables_before_the_run_is_an_error(tmp_path):
    sim = bs.Simulator(bs.SimConfig(quoter_config(tmp_path)))
    with pytest.raises(RuntimeError, match="run"):
        sim.tables()
    with pytest.raises(RuntimeError, match="run the simulator first"):
        Results.from_simulator(sim)


def _norm(column, value, fixed_columns):
    """A CSV string or a DataFrame value, as one comparable value (None for empty)."""
    if value is None or value == "":
        return None
    if column.endswith("_ns"):
        return int(value)
    if column in fixed_columns:
        return Decimal(value).normalize() if isinstance(value, str) else value.normalize()
    return str(value)


@pytest.mark.parametrize("which", ["quoter", "requests"])
def test_polars_frames_equal_the_csvs(which, request):
    from research.sim.results import _FIXED

    res, out = request.getfixturevalue(which)
    frames = res.to_polars()
    assert set(frames) == set(TABLES)
    for t in TABLES:
        with open(out / f"{t}.csv", newline="", encoding="utf-8") as f:
            rows = list(csv.reader(f))
        header, rows = rows[0], rows[1:]
        df = frames[t]
        assert df.columns == header, t
        assert df.height == len(rows), t
        fixed = set(_FIXED[t])
        for name in header:
            col = header.index(name)
            want = [_norm(name, row[col], fixed) for row in rows]
            series = df[name].dt.epoch("ns") if name.endswith("_ns") else df[name]  # ns exactly
            got = [_norm(name, v, fixed) for v in series.to_list()]
            assert got == want, f"{t}.{name}"
    if which == "requests":
        # the strategy reached every kind of row
        requests = set(frames["orders"]["request"].to_list())
        assert requests == {"new", "amend", "replace", "cancel"}
        assert frames["orders"]["new_price"].null_count() < frames["orders"].height
        assert frames["request_rejects"].height > 0 and frames["request_rejects"]["text"][0]
        assert {"maker", "taker"} >= set(frames["exec_reports"]["liquidity"].drop_nulls().to_list()) != set()


def test_polars_dtypes_and_fixed_modes(quoter):
    res, _ = quoter
    df = res.to_polars("exec_reports")
    assert df.schema["exchange_ns"] == pl.Datetime("ns", "UTC")
    assert df.schema["price"] == pl.Decimal(38, 8)
    assert df.schema["side"] == pl.String
    as_int = res.to_polars("exec_reports", fixed="int")
    as_float = res.to_polars("exec_reports", fixed="float")
    assert as_int["price"].to_list() == res.exec_reports["price"].tolist()
    assert as_float["price"][0] == pytest.approx(res.exec_reports["price"][0] / 1e8)
    pnl = res.to_polars("positions")["pnl"][-1]
    assert str(pnl.normalize()) == res.meta["sessions"][0]["exchange_account"]["pnl"]
    with pytest.raises(ValueError, match="fixed"):
        res.to_polars(fixed="double")
    with pytest.raises(ValueError, match="unknown table"):
        res.to_polars("trades")
