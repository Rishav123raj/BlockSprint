"""PassiveQuoter in Python (P4.7): a line-for-line port of src/sim/strategies/passive_quoter.cpp.

It joins the best bid and the best ask with a fixed size, re-quoting on a timer. Every refresh it looks at the
book it sees and its open orders:
  - one quote per side at the observed best price, as post-only LIMIT_MAKER (or plain LIMIT);
  - a quote at another price is replaced (losing its queue place, as on Binance);
  - a side whose next fill would take the position beyond max_position is not quoted (its quote is cancelled);
  - while the book is invalid (a gap) every quote is cancelled;
  - a quote with a request in flight is left alone until the reply comes.

Given the same config it sends the same requests at the same times as the C++ strategy, so a run gives the
same output files and hash (tests/python/test_parity.py). Keep the two in step: a change to one is a change to
both.

Params as the C++ one: size ("0.001"), refresh_ms (500), max_position ("0.01", absolute, base units),
post_only (True).
"""

from __future__ import annotations

import math
from typing import Any

import blocksprint_sim as bs
from research.strategies.base import Context, OpenOrder, Strategy

_PENDING = (bs.OrderStatus.PENDING_NEW, bs.OrderStatus.PENDING_CANCEL, bs.OrderStatus.PENDING_REPLACE)


def _decimal(key: str, value: Any) -> int:
    """A positive exact decimal: a string such as "0.001", or a number (as the C++ params read it)."""
    if isinstance(value, bool) or not isinstance(value, (str, int, float)):
        raise ValueError(f"{key}: expected a decimal such as \"0.001\", got {value!r}")
    text = value if isinstance(value, str) else repr(value)
    try:
        fixed = bs.to_fixed(text)
    except ValueError:
        fixed = 0
    if fixed <= 0:
        raise ValueError(f"{key}: expected a positive decimal with at most 8 places, got {text}")
    return fixed


class PassiveQuoter(Strategy):
    name = "passive_quoter_py"
    callback_mode = "timer_only"  # it reads the book on its timer only

    def __init__(self, size: str = "0.001", refresh_ms: float = 500, max_position: str = "0.01",
                 post_only: bool = True):
        self.size = _decimal("size", size)
        self.max_position = _decimal("max_position", max_position)
        if isinstance(refresh_ms, bool) or not isinstance(refresh_ms, (int, float)) or not refresh_ms >= 0:
            raise ValueError(f"refresh_ms: expected milliseconds >= 0, got {refresh_ms!r}")
        self.refresh_ns = math.floor(refresh_ms * 1e6 + 0.5)  # as StrategyParams::millis: llround, halves up
        if self.refresh_ns == 0:
            raise ValueError("refresh_ms: must be > 0")
        if self.max_position < self.size:
            raise ValueError("max_position: must be at least size")
        if not isinstance(post_only, bool):
            raise ValueError(f"post_only: expected true or false, got {post_only!r}")
        self.post_only = post_only
        self.refreshes = 0

    @classmethod
    def from_params(cls, params: dict[str, Any]) -> PassiveQuoter:
        """From a config's strategy.params (the C++ strategy's parameters); unknown keys are refused."""
        unknown = set(params) - {"size", "refresh_ms", "max_position", "post_only"}
        if unknown:
            raise ValueError(f"unknown parameter(s): {', '.join(sorted(unknown))}")
        return cls(**params)

    def on_start(self, ctx: Context) -> None:
        self._requote(ctx)
        ctx.set_timer(self.refresh_ns)

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        self._requote(ctx)
        ctx.set_timer(self.refresh_ns)

    def on_gap(self, ctx: Context) -> None:
        self._requote(ctx)

    def _requote(self, ctx: Context) -> None:
        if not ctx.can_trade():
            return
        self.refreshes += 1
        book = ctx.book()
        position = ctx.account().position
        bid = ask = None
        if book.valid():
            b = book.best_bid()
            if b is not None and position + self.size <= self.max_position:
                bid = b.price
            a = book.best_ask()
            if a is not None and position - self.size >= -self.max_position:
                ask = a.price
        orders = ctx.open_orders()  # one copy for both sides: requests below don't change it
        self._quote_side(ctx, orders, bs.Side.BUY, bid)
        self._quote_side(ctx, orders, bs.Side.SELL, ask)

    def _quote_side(self, ctx: Context, orders: dict[str, OpenOrder], side: bs.Side, target: int | None) -> None:
        """target: the price to quote at, or None for no quote on this side."""
        mine = []
        for o in orders.values():  # by order ID, as the C++ std::map
            if o.side != side:
                continue
            if o.status in _PENDING:
                return  # wait for the exchange's answer
            mine.append(o)
        for o in mine[0 if target is None else 1:]:
            ctx.cancel(o.id)
        if target is None:
            return
        if not mine:
            ctx.send(side, bs.OrderType.LIMIT_MAKER if self.post_only else bs.OrderType.LIMIT, target, self.size)
        elif mine[0].price != target:
            ctx.replace(mine[0].id, target, self.size)
