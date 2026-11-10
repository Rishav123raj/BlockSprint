"""The Python strategy base class and the typed view of what the simulator hands it (P4.6).

    from research.strategies import Strategy, Context, Book, ExecReport
    import blocksprint_sim as bs

    class BestBid(Strategy):
        callback_mode = "timer_only"           # Simulator / Results.run use it unless told otherwise

        def __init__(self, size: str = "0.001"):
            self.size = bs.to_fixed(size)

        def on_start(self, ctx: Context) -> None:
            ctx.set_timer(1_000_000_000)

        def on_timer(self, ctx: Context, timer_id: int) -> None:
            book = ctx.book()
            if ctx.can_trade() and book.valid() and (bid := book.best_bid()):
                ctx.send(bs.Side.BUY, bs.OrderType.LIMIT_MAKER, bid.price, self.size)
            ctx.set_timer(1_000_000_000)

    results = BestBid().run(config)            # research.sim.results.Results

Callbacks a subclass doesn't override cost nothing: the defaults are marked, and the C++ adapter never takes
the GIL for them. A method named on_* that isn't a callback (a typo such as on_fill) is an error when the
class is defined, rather than a callback that silently never runs.

The Protocol classes (Context, Book, ExecReport, ...) describe the C++ objects for type checkers and editors.
They cost nothing at run time: the objects passed in are blocksprint_sim's own. tests/python/test_strategy_base.py
checks that every member described here exists on the bound class.

Units: prices, quantities and money are integers in 1e-8 units (bs.to_fixed("0.001") == 100_000); times are
ns since the epoch. ctx and book are valid during the callback only: don't keep them.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Callable, Literal, Protocol, TypeVar

import blocksprint_sim as bs

if TYPE_CHECKING:
    from research.sim.results import Results

CallbackMode = Literal["every_event", "on_change", "timer_only"]
CALLBACK_MODES: tuple[str, ...] = ("every_event", "on_change", "timer_only")
CALLBACKS: tuple[str, ...] = ("on_start", "on_book", "on_trade", "on_exec", "on_reject", "on_timer", "on_gap",
                              "on_end")


# ---- what the simulator passes (the C++ objects, described for type checkers) ----

class BookLevel(Protocol):
    """One price level."""
    price: int
    qty: int


class TradeEvent(Protocol):
    """A trade of the recorded data."""
    ts_local: int         # when the recorder received it (the simulation's exchange time)
    ts_exch: int          # Binance's timestamp
    price: int
    qty: int
    aggressor: bs.Side    # BUY: a buyer lifted the ask


class Book(Protocol):
    """The market as the strategy sees it, after feed latency (bs.ObservedBook). Valid during the callback."""

    @property
    def tick_size(self) -> int: ...
    @property
    def lot_size(self) -> int: ...

    def valid(self) -> bool:
        """A snapshot established the book and no gap has happened since."""
        ...

    def best_bid(self) -> BookLevel | None: ...
    def best_ask(self) -> BookLevel | None: ...

    def qty(self, side: bs.Side, price: int) -> int:
        """The quantity at a price (0 if none)."""
        ...

    def top(self, side: bs.Side, n: int) -> list[BookLevel]:
        """The best n levels of a side, best first."""
        ...

    def levels(self, side: bs.Side) -> int: ...
    def last_trade(self) -> TradeEvent | None: ...

    def data_time(self) -> int:
        """ts_local of the newest data applied."""
        ...

    def seen_at(self) -> int:
        """Sim time the newest data reached the strategy."""
        ...


class ExecReport(Protocol):
    """A report for one of the strategy's orders."""
    seq: int
    timestamp: int        # exchange time
    type: bs.ExecType
    reason: bs.ExecReason
    text: str
    order_id: str
    owner_id: str
    symbol: str
    side: bs.Side
    order_type: bs.OrderType
    price: int
    quantity: int         # the order's quantity as last set
    cum_qty: int
    leaves_qty: int       # 0 in a terminal report
    cum_fee: int
    last_px: int          # FILL only
    last_qty: int         # FILL only
    liquidity: bs.Liquidity  # FILL only
    trade_id: str         # FILL only
    fee: int              # FILL only


class RequestReject(Protocol):
    """A refused cancel, amend or replace."""
    request: bs.Request
    order_id: str
    reason: bs.ExecReason  # NONE: the order isn't open (unknown, filled or cancelled)
    text: str
    exchange_time: int


class Account(Protocol):
    """Position and money (a copy)."""
    position: int
    cash: int
    fees: int
    base_volume: int
    quote_volume: int
    fills: int
    maker_fills: int
    taker_fills: int

    def pnl(self, mark: int) -> int:
        """cash + position x mark - fees, in 1e-8 quote units."""
        ...


class OpenOrder(Protocol):
    """An order as the strategy knows it (a copy)."""
    id: str
    side: bs.Side
    type: bs.OrderType
    price: int
    quantity: int
    filled: int
    leaves: int
    status: bs.OrderStatus
    sent_at: int


class Context(Protocol):
    """The strategy's connection to the exchange (bs.Context). Valid during the callback."""

    def now(self) -> int:
        """Sim time of the callback (ns since the epoch)."""
        ...

    def symbol(self) -> str: ...
    def owner(self) -> str: ...

    def book(self) -> Book:
        """The market as the strategy sees it (delayed by feed latency)."""
        ...

    def can_trade(self) -> bool:
        """False during the warm-up: send / cancel / amend / replace raise RuntimeError then. Timers work."""
        ...

    def send(self, side: bs.Side, type: bs.OrderType, price: int, quantity: int,
             stp: bs.StpPolicy = bs.StpPolicy.CANCEL_MAKER) -> str:
        """Sends a new order; returns its ID. A bad order comes back as a REJECTED report."""
        ...

    def cancel(self, order_id: str) -> None: ...

    def amend(self, order_id: str, new_total: int, new_price: int | None = None) -> None:
        """New total quantity (filled + open, as on Binance), optionally a new price."""
        ...

    def replace(self, order_id: str, price: int, quantity: int) -> None:
        """Cancel and send under the same ID (loses queue priority); refused if the order is no longer open."""
        ...

    def account(self) -> Account:
        """The account as the strategy knows it (a copy)."""
        ...

    def open_orders(self) -> dict[str, OpenOrder]:
        """Order ID -> OpenOrder, as the strategy knows them (a copy)."""
        ...

    def set_timer(self, delay_ns: int) -> int:
        """on_timer after delay_ns (no latency); returns the timer ID."""
        ...

    def cancel_timer(self, timer_id: int) -> None: ...


# ---- the base class ----

F = TypeVar("F", bound=Callable[..., Any])


def _default(fn: F) -> F:
    fn._bs_default = True  # type: ignore[attr-defined]  # the C++ adapter skips methods marked so
    return fn


class Strategy:
    """Base class for Python strategies. Override the callbacks you need; the rest cost nothing.

    Class attributes:
        name           shown in run_meta.json and the results (default: the class name)
        callback_mode  every_event | on_change | timer_only (default every_event): which book and trade
                       updates reach on_book / on_trade (docs/dev_setup.md)
    """

    name: str = "Strategy"
    callback_mode: CallbackMode = "every_event"

    def __init_subclass__(cls, **kwargs: Any) -> None:
        super().__init_subclass__(**kwargs)
        if "name" not in cls.__dict__:
            cls.name = cls.__name__
        if cls.callback_mode not in CALLBACK_MODES:
            raise TypeError(f"{cls.__name__}.callback_mode: expected one of {', '.join(CALLBACK_MODES)}, "
                            f"got {cls.callback_mode!r}")
        for attr in cls.__dict__:
            if attr.startswith("on_") and attr not in CALLBACKS:
                raise TypeError(f"{cls.__name__}.{attr} is not a callback (the callbacks are {', '.join(CALLBACKS)})")

    @_default
    def on_start(self, ctx: Context) -> None:
        """At the start of the window (before any data reaches the strategy)."""

    @_default
    def on_book(self, ctx: Context, book: Book) -> None:
        """A depth update or snapshot reached the strategy; book is ctx.book()."""

    @_default
    def on_trade(self, ctx: Context, trade: TradeEvent) -> None:
        """A trade of the data reached the strategy."""

    @_default
    def on_exec(self, ctx: Context, report: ExecReport) -> None:
        """A report for one of the strategy's orders: ack, fill, amended, cancelled or rejected."""

    @_default
    def on_reject(self, ctx: Context, reject: RequestReject) -> None:
        """The exchange refused a cancel, amend or replace."""

    @_default
    def on_timer(self, ctx: Context, timer_id: int) -> None:
        """A timer set with ctx.set_timer fired."""

    @_default
    def on_gap(self, ctx: Context) -> None:
        """Data was lost: the book is invalid until the next snapshot (on_book with book.valid())."""

    @_default
    def on_end(self, ctx: Context) -> None:
        """At the end of the window."""

    def run(self, config: bs.SimConfig | dict[str, Any], overrides: list[str] | tuple[str, ...] = ()) -> Results:
        """Runs this strategy on a config (a SimConfig, or a dict) with its callback_mode; returns the Results."""
        from research.sim.results import Results

        return Results.run(config, self, self.callback_mode, overrides)
