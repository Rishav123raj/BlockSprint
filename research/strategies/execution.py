"""Execution (P5.10): work a parent order of a fixed quantity over time, TWAP or POV.

    Executor(side="buy", quantity="1", mode="twap", duration_ms=3_000_000)            # evenly over 50 min
    Executor(side="sell", quantity="0.5", mode="pov", participation=0.05)            # 5% of the market's volume
    Executor(side="buy", quantity="1", mode="twap", duration_ms=600_000, style="passive")

Every `interval_ms` (default 1 s) it compares what it has done with its schedule and sends the shortfall:

* **twap:** the target at time t is quantity x (t - start) / duration, from the end of the warm-up; it is done by
  start + duration.
* **pov:** the target is participation x the market's traded volume since the start (the data's trades, which
  don't include the strategy's own fills), until the quantity is done.

Style:

* **market** (default): the shortfall goes as a MARKET order (a taker; it pays the spread).
* **passive:** one post-only LIMIT_MAKER rests at the strategy's own best price (the bid for a buy), for up to
  `max_behind` of the quantity (default 0.1) ahead of the schedule. It is re-priced by replace only when that
  price moves or it must shrink, since a replace loses the queue place. If the strategy falls more than
  `max_behind` behind the schedule, the excess goes as a MARKET order; at the deadline (twap) everything left
  does.

A slice smaller than `min_slice` (default "0.0001", above Binance's 5 USDT minimum notional for BTCUSDT) waits
until it is larger, except the last. Quantities are rounded down to the lot size. In-flight orders count as done
until their reports come back.

Params: side ("buy" / "sell"), quantity, mode ("twap" / "pov"), duration_ms (twap), participation (pov, 0-1],
interval_ms (1000), style ("market" / "passive"), max_behind (0.1), min_slice ("0.0001").
"""

from __future__ import annotations

import math

import blocksprint_sim as bs
from research.strategies.base import Context, ExecReport, Strategy, TradeEvent
from research.strategies.passive_quoter import _decimal

_PENDING = (bs.OrderStatus.PENDING_NEW, bs.OrderStatus.PENDING_CANCEL, bs.OrderStatus.PENDING_REPLACE)


def _millis(key: str, value: float, positive: bool = True) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not value >= 0 or (positive and value == 0):
        raise ValueError(f"{key}: expected milliseconds {'> 0' if positive else '>= 0'}, got {value!r}")
    return math.floor(value * 1e6 + 0.5)


def _side(value: str) -> bs.Side:
    if value not in ("buy", "sell"):
        raise ValueError(f"side: expected 'buy' or 'sell', got {value!r}")
    return bs.Side.BUY if value == "buy" else bs.Side.SELL


class Executor(Strategy):
    name = "executor"
    callback_mode = "on_change"  # POV needs every trade; the schedule runs on a timer

    def __init__(self, side: str = "buy", quantity: str = "0.01", mode: str = "twap", duration_ms: float = 600_000,
                 participation: float = 0.05, interval_ms: float = 1000, style: str = "market",
                 max_behind: float = 0.1, min_slice: str = "0.0001"):
        self.side = _side(side)
        self.quantity = _decimal("quantity", quantity)
        if mode not in ("twap", "pov"):
            raise ValueError(f"mode: expected 'twap' or 'pov', got {mode!r}")
        self.mode = mode
        self.duration_ns = _millis("duration_ms", duration_ms)
        if isinstance(participation, bool) or not isinstance(participation, (int, float)) or not 0 < participation <= 1:
            raise ValueError(f"participation: expected a fraction in (0, 1], got {participation!r}")
        self.participation = float(participation)
        self.interval_ns = _millis("interval_ms", interval_ms)
        if style not in ("market", "passive"):
            raise ValueError(f"style: expected 'market' or 'passive', got {style!r}")
        self.style = style
        if isinstance(max_behind, bool) or not isinstance(max_behind, (int, float)) or not 0 <= max_behind <= 1:
            raise ValueError(f"max_behind: expected a fraction in [0, 1], got {max_behind!r}")
        self.max_behind = float(max_behind)
        self.min_slice = _decimal("min_slice", min_slice)
        self.start_ns: int | None = None
        self.market_volume = 0   # the data's traded quantity since the start (1e-8 units)
        self.filled = 0
        self.slices = 0

    # ---- callbacks ----

    def on_start(self, ctx: Context) -> None:
        ctx.set_timer(self.interval_ns)

    def on_trade(self, ctx: Context, trade: TradeEvent) -> None:
        if self.start_ns is not None:
            self.market_volume += trade.qty

    def on_exec(self, ctx: Context, report: ExecReport) -> None:
        if report.type == bs.ExecType.FILL:
            self.filled += report.last_qty

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        if ctx.can_trade():
            if self.start_ns is None:
                self.start_ns = ctx.now()
            self._work(ctx)
        if self.filled < self.quantity:
            ctx.set_timer(self.interval_ns)

    # ---- the schedule ----

    def target(self, now: int) -> int:
        """Quantity (1e-8 units) that should be done by now."""
        if self.start_ns is None:
            return 0
        if self.mode == "twap":
            frac = min(max((now - self.start_ns) / self.duration_ns, 0.0), 1.0)
            return min(self.quantity, math.floor(self.quantity * frac))
        return min(self.quantity, math.floor(self.market_volume * self.participation))

    def _lots(self, qty: int, lot: int) -> int:
        return qty // lot * lot if lot > 0 else qty

    def _work(self, ctx: Context) -> None:
        book = ctx.book()
        lot = book.lot_size
        orders = [o for o in ctx.open_orders().values() if o.side == self.side]
        if any(o.status in _PENDING for o in orders):
            return  # wait for the exchange's answers
        resting = sum(o.leaves for o in orders)
        remaining = self.quantity - self.filled
        now = ctx.now()
        deadline = self.mode == "twap" and now >= self.start_ns + self.duration_ns
        target = self.quantity if deadline else self.target(now)
        if self.style == "market":
            want = self._lots(min(target - self.filled - resting, remaining - resting), lot)
            last = self._lots(remaining - resting, lot) == want
            if want > 0 and (want >= self.min_slice or last):
                ctx.send(self.side, bs.OrderType.MARKET, 0, want)
                self.slices += 1
            return
        # passive: one resting order at the own-side best, allowed to run up to `max_behind` ahead of the
        # schedule; it is re-priced only when the best price moves or it must shrink (a replace loses the queue)
        tol = math.floor(self.max_behind * self.quantity)
        behind = target - self.filled
        urgent = self._lots(remaining if deadline else behind - tol, lot)
        if urgent > 0 and (urgent >= self.min_slice or urgent == self._lots(remaining, lot)):
            for o in orders:
                ctx.cancel(o.id)
            ctx.send(self.side, bs.OrderType.MARKET, 0, urgent)
            self.slices += 1
            return
        level = book.best_bid() if self.side == bs.Side.BUY else book.best_ask()
        allowed = self._lots(min(remaining, behind + tol), lot)
        if not book.valid() or level is None or allowed < min(self.min_slice, self._lots(remaining, lot)) or allowed <= 0:
            for o in orders:
                ctx.cancel(o.id)
            return
        if not orders:
            ctx.send(self.side, bs.OrderType.LIMIT_MAKER, level.price, allowed)
            self.slices += 1
            return
        for o in orders[1:]:
            ctx.cancel(o.id)
        o = orders[0]
        if o.price != level.price or o.leaves > allowed:
            ctx.replace(o.id, level.price, allowed)
