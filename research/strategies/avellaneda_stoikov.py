"""Avellaneda-Stoikov market maker (P5.10): quotes around a reservation price that leans against the inventory.

From Avellaneda and Stoikov, "High-frequency trading in a limit order book" (2008), with a constant horizon
(the quotes don't widen or narrow as a session end approaches):

    reservation price   r = m - q * gamma * sigma^2 * tau
    total spread        d = gamma * sigma^2 * tau + (2 / gamma) * ln(1 + gamma / k)
    bid = r - d / 2,  ask = r + d / 2

* m: the mid the strategy sees; q: the inventory in units of the quote size (position / size, as the paper
  counts shares), so one fill moves the reservation price by gamma * sigma^2 * tau;
* sigma^2: the mid's variance per second (quote units^2 / s), estimated online: an exponentially weighted
  mean of (change in mid)^2 / (time between refreshes), half-life `vol_halflife_s`, starting at `sigma0`;
* gamma: risk aversion (larger: more skew and a wider spread); k: how fast fill intensity falls with
  distance from the mid (per quote unit; larger: tighter quotes); tau = `horizon_s`.

Quotes are post-only LIMIT_MAKER of `size`, the bid rounded down and the ask up to the tick, never closer than
`min_spread_ticks` to the mid's own side and never crossing the observed book (a quote that would cross is
moved to one tick inside the other side's best). A side whose next fill would take |position| beyond
`max_position` isn't quoted. Every `refresh_ms` it re-prices by replace (a moved quote loses its queue place,
as on Binance); while the book is invalid (a gap) it cancels everything.

Params: size ("0.001"), gamma (0.0005), k (100), horizon_s (30), sigma0 (2.0, quote units per sqrt second),
vol_halflife_s (60), refresh_ms (500), max_position ("0.01"), min_spread_ticks (1).
Defaults are a starting point for BTCUSDT at ~70,000 USDT, not a calibration: sweep gamma and k.
"""

from __future__ import annotations

import math

import blocksprint_sim as bs
from research.strategies.base import Context, OpenOrder, Strategy
from research.strategies.execution import _millis
from research.strategies.passive_quoter import _decimal

_PENDING = (bs.OrderStatus.PENDING_NEW, bs.OrderStatus.PENDING_CANCEL, bs.OrderStatus.PENDING_REPLACE)
SCALE = 100_000_000


def _positive(key: str, value: float, allow_zero: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not (value >= 0 if allow_zero else value > 0):
        raise ValueError(f"{key}: expected a number {'>= 0' if allow_zero else '> 0'}, got {value!r}")
    return float(value)


class AvellanedaStoikov(Strategy):
    name = "avellaneda_stoikov"
    callback_mode = "timer_only"  # it reads the book on its timer

    def __init__(self, size: str = "0.001", gamma: float = 0.0005, k: float = 100.0, horizon_s: float = 30.0,
                 sigma0: float = 2.0, vol_halflife_s: float = 60.0, refresh_ms: float = 500,
                 max_position: str = "0.01", min_spread_ticks: int = 1):
        self.size = _decimal("size", size)
        self.gamma = _positive("gamma", gamma)
        self.k = _positive("k", k)
        self.horizon_s = _positive("horizon_s", horizon_s)
        self.var = _positive("sigma0", sigma0) ** 2  # quote units^2 per second
        self.halflife_s = _positive("vol_halflife_s", vol_halflife_s)
        self.refresh_ns = _millis("refresh_ms", refresh_ms)
        self.max_position = _decimal("max_position", max_position)
        if self.max_position < self.size:
            raise ValueError("max_position: must be at least size")
        if isinstance(min_spread_ticks, bool) or not isinstance(min_spread_ticks, int) or min_spread_ticks < 0:
            raise ValueError(f"min_spread_ticks: expected an integer >= 0, got {min_spread_ticks!r}")
        self.min_spread_ticks = min_spread_ticks
        self.last_mid: float | None = None
        self.last_t: int | None = None
        self.quotes = 0

    # ---- the model ----

    def sigma2(self) -> float:
        return self.var

    def update_vol(self, mid: float, now: int) -> None:
        """Folds the mid's move since the last refresh into the variance estimate."""
        if self.last_mid is not None and self.last_t is not None and now > self.last_t:
            dt = (now - self.last_t) / 1e9
            w = 1.0 - 0.5 ** (dt / self.halflife_s)
            self.var += w * ((mid - self.last_mid) ** 2 / dt - self.var)
        self.last_mid, self.last_t = mid, now

    def quotes_for(self, mid: float, q: float) -> tuple[float, float]:
        """(bid, ask) in quote units for a mid and an inventory q (in units of size), before rounding."""
        risk = self.gamma * self.var * self.horizon_s
        reservation = mid - q * risk
        spread = risk + (2.0 / self.gamma) * math.log1p(self.gamma / self.k)
        return reservation - spread / 2, reservation + spread / 2

    # ---- callbacks ----

    def on_start(self, ctx: Context) -> None:
        ctx.set_timer(self.refresh_ns)

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        self._requote(ctx)
        ctx.set_timer(self.refresh_ns)

    def on_gap(self, ctx: Context) -> None:
        self.last_mid = self.last_t = None
        self._requote(ctx)

    def _requote(self, ctx: Context) -> None:
        book = ctx.book()
        b, a = (book.best_bid(), book.best_ask()) if book.valid() else (None, None)
        if b is not None and a is not None:
            self.update_vol((b.price + a.price) / 2 / SCALE, ctx.now())  # the vol estimate runs in the warm-up too
        if not ctx.can_trade():
            return
        orders = ctx.open_orders()
        if b is None or a is None:
            self._quote_side(ctx, orders, bs.Side.BUY, None)
            self._quote_side(ctx, orders, bs.Side.SELL, None)
            return
        tick = book.tick_size
        position = ctx.account().position
        mid = (b.price + a.price) / 2 / SCALE
        bid_f, ask_f = self.quotes_for(mid, position / self.size)
        bid = math.floor(bid_f * SCALE / tick) * tick
        ask = math.ceil(ask_f * SCALE / tick) * tick
        mid_fixed = (b.price + a.price) / 2
        bid = min(bid, math.floor((mid_fixed - self.min_spread_ticks * tick) / tick) * tick, a.price - tick)
        ask = max(ask, math.ceil((mid_fixed + self.min_spread_ticks * tick) / tick) * tick, b.price + tick)
        self.quotes += 1
        self._quote_side(ctx, orders, bs.Side.BUY, bid if position + self.size <= self.max_position else None)
        self._quote_side(ctx, orders, bs.Side.SELL, ask if position - self.size >= -self.max_position else None)

    def _quote_side(self, ctx: Context, orders: dict[str, OpenOrder], side: bs.Side, target: int | None) -> None:
        mine = []
        for o in orders.values():
            if o.side != side:
                continue
            if o.status in _PENDING:
                return
            mine.append(o)
        for o in mine[0 if target is None else 1:]:
            ctx.cancel(o.id)
        if target is None or target <= 0:
            return
        if not mine:
            ctx.send(side, bs.OrderType.LIMIT_MAKER, target, self.size)
        elif mine[0].price != target:
            ctx.replace(mine[0].id, target, self.size)
