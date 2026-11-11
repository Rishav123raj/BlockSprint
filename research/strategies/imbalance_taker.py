"""Order-book imbalance taker (P5.10): takes liquidity in the direction the book leans, then gets out.

    imbalance I = (bid quantity - ask quantity) / (bid quantity + ask quantity), over the best `levels` levels

On each change of the best bid or ask it sees:

* **flat** (and `cooldown_ms` past its last exit): I >= threshold buys `size` at market; I <= -threshold sells.
* **in a position:** it closes at market once `hold_ms` has passed, or earlier when the imbalance has turned
  against it (I <= -exit_threshold for a long, >= exit_threshold for a short).

One position at a time, of `size`; nothing new while an order is in flight. It is a taker: every trade pays
the spread and the taker fee, so it earns only if the imbalance predicts the move past those costs (the
markouts say).

Params: size ("0.001"), levels (1), threshold (0.6), exit_threshold (0.6), hold_ms (1000), cooldown_ms (1000).
"""

from __future__ import annotations

import blocksprint_sim as bs
from research.strategies.base import Book, Context, ExecReport, Strategy
from research.strategies.execution import _millis
from research.strategies.passive_quoter import _decimal


def _fraction(key: str, value: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 < value <= 1:
        raise ValueError(f"{key}: expected a number in (0, 1], got {value!r}")
    return float(value)


def imbalance(book: Book, levels: int) -> float | None:
    """(bid - ask) / (bid + ask) over the best `levels` levels of each side; None without both sides."""
    bids = book.top(bs.Side.BUY, levels)
    asks = book.top(bs.Side.SELL, levels)
    qb = sum(level.qty for level in bids)
    qa = sum(level.qty for level in asks)
    if not bids or not asks or qb + qa == 0:
        return None
    return (qb - qa) / (qb + qa)


class ImbalanceTaker(Strategy):
    name = "imbalance_taker"
    callback_mode = "on_change"  # a decision per change of the best prices or quantities

    def __init__(self, size: str = "0.001", levels: int = 1, threshold: float = 0.6, exit_threshold: float = 0.6,
                 hold_ms: float = 1000, cooldown_ms: float = 1000):
        self.size = _decimal("size", size)
        if isinstance(levels, bool) or not isinstance(levels, int) or levels < 1:
            raise ValueError(f"levels: expected an integer >= 1, got {levels!r}")
        self.levels = levels
        self.threshold = _fraction("threshold", threshold)
        self.exit_threshold = _fraction("exit_threshold", exit_threshold)
        self.hold_ns = _millis("hold_ms", hold_ms, positive=False)
        self.cooldown_ns = _millis("cooldown_ms", cooldown_ms, positive=False)
        self.in_flight = False
        self.entered_at: int | None = None
        self.exited_at: int | None = None
        self.entries = 0

    def on_book(self, ctx: Context, book: Book) -> None:
        if self.in_flight or not ctx.can_trade() or not book.valid():
            return
        i = imbalance(book, self.levels)
        if i is None:
            return
        now = ctx.now()
        position = ctx.account().position
        if position != 0:
            held = self.entered_at is not None and now - self.entered_at >= self.hold_ns
            against = i <= -self.exit_threshold if position > 0 else i >= self.exit_threshold
            if held or against:
                side = bs.Side.SELL if position > 0 else bs.Side.BUY
                ctx.send(side, bs.OrderType.MARKET, 0, abs(position))
                self.in_flight = True
            return
        if self.exited_at is not None and now - self.exited_at < self.cooldown_ns:
            return
        if i >= self.threshold or i <= -self.threshold:
            ctx.send(bs.Side.BUY if i > 0 else bs.Side.SELL, bs.OrderType.MARKET, 0, self.size)
            self.in_flight = True
            self.entries += 1

    def on_exec(self, ctx: Context, report: ExecReport) -> None:
        if report.leaves_qty != 0 and report.type not in (bs.ExecType.CANCELLED, bs.ExecType.REJECTED):
            return  # more reports to come for this order
        if report.type == bs.ExecType.ACK:
            return
        self.in_flight = False
        position = ctx.account().position
        if position == 0:
            self.exited_at, self.entered_at = report.timestamp, None
        elif self.entered_at is None:
            self.entered_at = report.timestamp
