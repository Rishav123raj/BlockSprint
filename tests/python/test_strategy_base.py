"""The Python Strategy base class and the typed facade (P4.6)."""

import typing
from pathlib import Path

import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.sim.results import Results
from research.strategies import (
    Account,
    Book,
    BookLevel,
    Context,
    ExecReport,
    OpenOrder,
    RequestReject,
    Strategy,
    TradeEvent,
)

FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
SECOND = 1_000_000_000


def config(tmp_path):
    return {"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps", "strategy": {"name": "null"},
            "output": {"dir": str(tmp_path)}}


@pytest.mark.parametrize("protocol, bound", [
    (Context, bs.Context), (Book, bs.ObservedBook), (BookLevel, bs.BookLevel), (TradeEvent, bs.TradeEvent),
    (ExecReport, bs.ExecReport), (RequestReject, bs.RequestReject), (Account, bs.Account),
    (OpenOrder, bs.OpenOrder),
])
def test_the_typed_facade_matches_the_bound_classes(protocol, bound):
    described = typing.get_protocol_members(protocol)
    public = {n for n in dir(bound) if not n.startswith("_")}
    assert described == public, (f"described only: {sorted(described - public)}; "
                                 f"bound only: {sorted(public - described)}")


class Timer(Strategy):
    """Only on_start and on_timer: every other callback is a base-class default."""

    callback_mode = "every_event"

    def __init__(self):
        self.timers = 0

    def on_start(self, ctx: Context) -> None:
        ctx.set_timer(SECOND)

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        self.timers += 1
        ctx.set_timer(SECOND)


def test_default_callbacks_are_never_called(tmp_path):
    s = Timer()
    meta = bs.Simulator(bs.SimConfig(config(tmp_path / "timer")), s).run()
    assert s.timers == 59
    assert meta["python_callbacks"] == 1 + s.timers  # on_start + timers: no book, trade, end... calls

    # Nothing overridden: no callback at all, and the C++ null strategy's results
    py = bs.Simulator(bs.SimConfig(config(tmp_path / "base")), Strategy()).run()
    cpp = bs.Simulator(bs.SimConfig(config(tmp_path / "cpp"))).run()
    assert py["python_callbacks"] == 0 and py["output_hash"] == cpp["output_hash"]


def test_name_and_callback_mode_come_from_the_class(tmp_path):
    class Books(Strategy):
        callback_mode = "timer_only"

        def __init__(self):
            self.books = 0

        def on_book(self, ctx: Context, book: Book) -> None:
            self.books += 1

    class Named(Books):
        name = "my-strategy"
        callback_mode = "on_change"

    assert Books.name == "Books" and Named.name == "my-strategy" and Strategy().name == "Strategy"

    timer_only = Books()
    res = timer_only.run(config(tmp_path / "a"))
    assert isinstance(res, Results) and timer_only.books == 0  # the class's timer_only
    assert res.meta["sessions"][0]["strategy"] == "Books"

    explicit = Books()
    bs.Simulator(bs.SimConfig(config(tmp_path / "b")), explicit, "every_event").run()  # an explicit mode wins
    on_change = Named()
    meta = Results.run(config(tmp_path / "c"), on_change).meta
    assert explicit.books > on_change.books > 0
    assert meta["python_strategy"] == "my-strategy"


def test_mistakes_fail_when_the_class_is_defined():
    with pytest.raises(TypeError, match="on_fill is not a callback"):
        class Typo(Strategy):
            def on_fill(self, ctx, report):
                pass

    with pytest.raises(TypeError, match="callback_mode"):
        class Mode(Strategy):
            callback_mode = "sometimes"


def test_a_typed_strategy_trades(tmp_path):
    class BestBid(Strategy):
        callback_mode = "timer_only"

        def __init__(self, size: str = "0.001"):
            self.size = bs.to_fixed(size)
            self.fills = 0
            self.final: Account | None = None

        def on_start(self, ctx: Context) -> None:
            ctx.set_timer(SECOND)

        def on_timer(self, ctx: Context, timer_id: int) -> None:
            book = ctx.book()
            if ctx.can_trade() and book.valid() and (bid := book.best_bid()):
                ctx.send(bs.Side.BUY, bs.OrderType.LIMIT_MAKER, bid.price, self.size)
            ctx.set_timer(SECOND)

        def on_exec(self, ctx: Context, report: ExecReport) -> None:
            if report.type == bs.ExecType.FILL:
                self.fills += 1

        def on_end(self, ctx: Context) -> None:
            self.final = ctx.account()

    s = BestBid()
    res = s.run(config(tmp_path))
    assert s.fills > 0 and s.final is not None and s.final.fills == s.fills
    assert res.meta["sessions"][0]["exchange_account"]["fills"] >= s.fills  # some may be in flight at the end
