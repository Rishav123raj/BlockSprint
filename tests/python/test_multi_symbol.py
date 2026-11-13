"""Multi-symbol runs (P6.7) end to end: a Python strategy buys BTC and hedges each fill by selling ETH of the
same notional, on 30 s of both symbols' Nov 10 data (committed fixtures). Per-symbol accounts, the symbol
column, narrowing a run to one symbol for the analytics, and determinism."""

from pathlib import Path

import pytest

import blocksprint_sim as bs  # noqa: F401  built by uv sync; a missing module is a failure, not a skip
import polars as pl

from research.analytics import tca
from research.analytics.pnl import decompose
from research.sim.results import Results
from research.strategies import Context, ExecReport, Strategy

DATA = Path(__file__).resolve().parents[2] / "tests" / "sim" / "data"
BTC = DATA / "btcusdt_2026-11-10_0200_30s.bse"
ETH = DATA / "ethusdt_2026-11-10_0200_30s.bse"
SECOND = 1_000_000_000
SCALE = 100_000_000
BTC_LOT, ETH_LOT = 1_000, 10_000  # 0.00001 BTC, 0.0001 ETH (1e-8 units)


class Hedger(Strategy):
    """Every 5 s buys 0.001 BTC at market; each BTC fill is hedged at once with a market sell of ETH worth
    the same at the ETH bid it sees."""
    callback_mode = "timer_only"

    def __init__(self):
        self.symbols: list[str] = []
        self.hedges = 0

    def on_start(self, ctx: Context) -> None:
        self.symbols = ctx.symbols()
        ctx.set_timer(SECOND)

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        if ctx.can_trade() and ctx.book("BTCUSDT").valid():
            ctx.send(bs.Side.BUY, bs.OrderType.MARKET, 0, 100_000)  # the primary: BTCUSDT
        ctx.set_timer(5 * SECOND)

    def on_exec(self, ctx: Context, report: ExecReport) -> None:
        if report.type != bs.ExecType.FILL or report.symbol != "BTCUSDT":
            return
        bid = ctx.book("ETHUSDT").best_bid()
        notional = report.last_qty * report.last_px // SCALE
        qty = notional * SCALE // bid.price // ETH_LOT * ETH_LOT
        if qty > 0:
            ctx.send(bs.Side.SELL, bs.OrderType.MARKET, 0, qty, symbol="ETHUSDT")
            self.hedges += 1


def config(tmp_path, name):
    return {"dataset": [str(BTC), str(ETH)], "latency_preset": "binance_spot_tokyo_vps",
            "strategy": {"name": "null"}, "output": {"dir": str(tmp_path / name), "sample_ms": 1000}}


@pytest.fixture(scope="module")
def run(tmp_path_factory):
    s = Hedger()
    res = Results.run(config(tmp_path_factory.mktemp("multi"), "hedge"), s, "timer_only")
    return s, res


def test_the_run_has_both_symbols_and_hedges_every_fill(run):
    s, res = run
    assert s.symbols == ["BTCUSDT", "ETHUSDT"]
    assert res.symbols == ["BTCUSDT", "ETHUSDT"] and res.multi_symbol
    assert res.meta["config"]["symbol"] == ["BTCUSDT", "ETHUSDT"]
    assert [x["symbol"] for x in res.meta["data"]["symbols"]] == ["BTCUSDT", "ETHUSDT"]
    (session,) = res.meta["sessions"]
    btc = session["accounts"]["BTCUSDT"]["exchange_account"]
    eth = session["accounts"]["ETHUSDT"]["exchange_account"]
    assert float(btc["position"]) == pytest.approx(0.006)  # 6 buys at 1, 6, ..., 26 s
    assert btc["fills"] >= 6 and s.hedges == btc["fills"]
    # The hedge: short ETH worth what was bought, within one ETH lot at the price it was sold at
    eth_price = float(res.meta["symbols"][1]["final_mark"])
    assert float(eth["position"]) < 0
    assert -float(eth["position"]) * eth_price == pytest.approx(-float(btc["cash"]), rel=0.01)
    assert session["exchange_account"] == btc  # the top-level account is the primary's


def test_tables_carry_the_symbol_and_narrow_to_one(run):
    _, res = run
    fills = res.to_polars("exec_reports").filter(pl.col("type") == "fill")
    assert fills.columns[2:4] == ["owner", "symbol"]
    assert set(fills["symbol"]) == {"BTCUSDT", "ETHUSDT"}
    pos = res.to_polars("positions")
    assert pos.group_by("symbol").len().sort("symbol")["len"].to_list() == [pos.height // 2] * 2

    eth = res.for_symbol("ETHUSDT")
    assert not eth.multi_symbol and eth.symbols == ["ETHUSDT"]
    assert "symbol" not in eth.to_polars("exec_reports").columns
    assert (eth.exec_reports["symbol"] == 1).all() and len(eth.exec_reports) > 0
    assert eth.meta["data"]["file"].endswith("ethusdt_2026-11-10_0200_30s.bse")
    assert eth.meta["sessions"][0]["exchange_account"] == res.meta["sessions"][0]["accounts"]["ETHUSDT"]["exchange_account"]
    with pytest.raises(ValueError, match="unknown symbol"):
        res.for_symbol("XRPUSDT")


def test_the_analytics_take_one_symbol_at_a_time(run):
    _, res = run
    with pytest.raises(ValueError, match="for_symbol"):
        decompose(res)
    btc = decompose(res.for_symbol("BTCUSDT")).to_dicts()[0]
    eth = decompose(res.for_symbol("ETHUSDT")).to_dicts()[0]
    assert btc["base_volume"] == pytest.approx(0.006)
    assert eth["base_volume"] > 0
    # Market orders: every fill is a taker fill against that symbol's own data
    for sym in ("BTCUSDT", "ETHUSDT"):
        costs = tca.fill_costs(res.for_symbol(sym), "5s")
        assert costs.height > 0


def test_same_config_same_outputs(run, tmp_path):
    _, res = run
    again = Results.run(config(tmp_path, "again"), Hedger(), "timer_only")
    assert again.output_hash == res.output_hash


def test_one_file_of_a_pair_is_a_one_symbol_run(tmp_path):
    cfg = config(tmp_path, "one")
    cfg["dataset"] = [str(ETH)]
    res = Results.run(cfg)
    assert res.symbols == ["ETHUSDT"] and not res.multi_symbol
    assert "symbols" not in res.meta and "accounts" not in res.meta["sessions"][0]
    assert res.for_symbol("ETHUSDT") is res
