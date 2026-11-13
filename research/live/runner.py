"""Live strategy runner (P7.3): a research strategy, unchanged, in paper mode against the BlockSprint server.

The strategy is the same class the simulator runs (research.strategies.Strategy, the same callbacks and Context
API); LiveContext implements that Context over the server's WebSockets, as the simulator's gateway session does
over its event queue:

* /ws/orders      the strategy's requests (send, cancel, amend, replace) and the server's replies;
* /ws/executions  every report for the runner's owner: they change what the strategy knows (its open orders and
                  accounts) and reach on_exec, as in the simulator;
* /ws/market-data the book the strategy sees (ctx.book(symbol)): the server's top 10 levels at most every
                  100 ms, without the strategy's own resting orders (the simulator's observed book shows the
                  data, not the strategy's orders); a ``gap`` event makes it invalid and calls on_gap;
* /ws/trades      the market's trades (the market feed's tape, P7.1) for on_trade.

Times are the wall clock (ns since the epoch); prices, quantities and money are integers in 1e-8 units, enums
are blocksprint_sim's. What differs from the simulator, by construction: latency is real, the book shows 10 levels
(sampled every 100 ms), and a replace of an order the strategy doesn't know as open is refused locally (the
server would treat it as a new order; the simulator's gateway refuses it).

Every request, report, refusal and a per-second top-of-book sample are written to ``session.jsonl`` in the output
directory, with ``session_meta.json``, for the paper-vs-sim parity protocol (P7.4).

Usage::

    uv run python -m research.live.runner --strategy research.strategies.passive_quoter:PassiveQuoter \\
        --params '{"size": "0.001", "refresh_ms": 500}' --symbols BTC-USDT --duration 600 --out results/paper/run1
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import time
from collections import deque
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path
from typing import Any

import blocksprint_sim as bs

log = logging.getLogger("live_runner")

SCALE = 100_000_000
_PENDING = (bs.OrderStatus.PENDING_NEW, bs.OrderStatus.PENDING_CANCEL, bs.OrderStatus.PENDING_REPLACE)


# ---- conversions ----

def fixed(text: str | None) -> int:
    """'68909.650000' -> 6890965000000 (exact; at most 8 decimals)."""
    if text is None or text == "":
        return 0
    return int(Decimal(text) * SCALE)


def decimal_text(v: int) -> str:
    sign = "-" if v < 0 else ""
    v = abs(v)
    return f"{sign}{v // SCALE}.{v % SCALE:08d}"


def iso_ns(text: str | None) -> int:
    """'2026-11-13T18:36:27.833329Z' -> ns since the epoch."""
    if not text:
        return 0
    head, _, frac = text.rstrip("Z").partition(".")
    dt = datetime.strptime(head, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
    return int(dt.timestamp()) * 1_000_000_000 + int((frac + "000000000")[:9])


def enum(e: Any, text: str | None, default: Any = None) -> Any:
    if not text:
        return default
    return e.__members__[text.upper()]


def notional(price: int, qty: int) -> int:
    """price x quantity in 1e-8 units, rounded half away from zero (fees.hpp notional_for)."""
    n = price * qty
    return (n + SCALE // 2) // SCALE if n >= 0 else -((-n + SCALE // 2) // SCALE)


# ---- what the strategy is handed (the shapes research.strategies.base describes) ----

@dataclass(frozen=True)
class Level:
    price: int
    qty: int


@dataclass(frozen=True)
class TradeEvent:
    ts_local: int
    ts_exch: int
    price: int
    qty: int
    aggressor: Any
    symbol: int


@dataclass
class ExecReport:
    seq: int = 0
    timestamp: int = 0
    type: Any = None
    reason: Any = bs.ExecReason.NONE
    text: str = ""
    order_id: str = ""
    owner_id: str = ""
    symbol: str = ""
    side: Any = None
    order_type: Any = None
    price: int = 0
    quantity: int = 0
    cum_qty: int = 0
    leaves_qty: int = 0
    cum_fee: int = 0
    last_px: int = 0
    last_qty: int = 0
    liquidity: Any = bs.Liquidity.NONE
    trade_id: str = ""
    fee: int = 0

    @classmethod
    def from_json(cls, d: dict[str, Any]) -> ExecReport:
        return cls(seq=int(d.get("seq", 0)), timestamp=iso_ns(d.get("timestamp")), type=enum(bs.ExecType, d.get("type")),
                   reason=enum(bs.ExecReason, d.get("reason"), bs.ExecReason.NONE), text=d.get("text", ""),
                   order_id=d.get("order_id", ""), owner_id=d.get("owner", ""), symbol=d.get("symbol", ""),
                   side=enum(bs.Side, d.get("side")), order_type=enum(bs.OrderType, d.get("order_type")),
                   price=fixed(d.get("price")), quantity=fixed(d.get("quantity")), cum_qty=fixed(d.get("cum_qty")),
                   leaves_qty=fixed(d.get("leaves_qty")), cum_fee=fixed(d.get("cum_fee")), last_px=fixed(d.get("last_px")),
                   last_qty=fixed(d.get("last_qty")), liquidity=enum(bs.Liquidity, d.get("liquidity"), bs.Liquidity.NONE),
                   trade_id=str(d.get("trade_id", "")), fee=fixed(d.get("fee")))


@dataclass
class RequestReject:
    request: Any
    order_id: str
    symbol: str
    reason: Any = bs.ExecReason.NONE
    text: str = ""
    exchange_time: int = 0


@dataclass
class Account:
    position: int = 0
    cash: int = 0
    fees: int = 0
    base_volume: int = 0
    quote_volume: int = 0
    fills: int = 0
    maker_fills: int = 0
    taker_fills: int = 0

    def pnl(self, mark: int) -> int:
        value = notional(mark, self.position) if self.position >= 0 else -notional(mark, -self.position)
        return self.cash + value - self.fees

    def apply_fill(self, r: ExecReport) -> None:
        n = notional(r.last_px, r.last_qty)
        if r.side == bs.Side.BUY:
            self.position += r.last_qty
            self.cash -= n
        else:
            self.position -= r.last_qty
            self.cash += n
        self.fees += r.fee
        self.base_volume += r.last_qty
        self.quote_volume += n
        self.fills += 1
        if r.liquidity == bs.Liquidity.MAKER:
            self.maker_fills += 1
        elif r.liquidity == bs.Liquidity.TAKER:
            self.taker_fills += 1


@dataclass
class OpenOrder:
    id: str
    symbol: str
    side: Any
    type: Any
    price: int
    quantity: int
    filled: int = 0
    leaves: int = 0
    status: Any = bs.OrderStatus.PENDING_NEW
    sent_at: int = 0


class LiveBook:
    """The market as the strategy sees it: the server's top levels without the strategy's own resting orders."""

    def __init__(self, symbol: str, index: int, tick_size: int, lot_size: int):
        self._symbol = symbol
        self._index = index
        self._tick = tick_size
        self._lot = lot_size
        self._bids: list[Level] = []
        self._asks: list[Level] = []
        self._valid = False
        self._gapped = False
        self._last_trade: TradeEvent | None = None
        self._data_time = 0

    @property
    def symbol(self) -> str:
        return self._symbol

    @property
    def tick_size(self) -> int:
        return self._tick

    @property
    def lot_size(self) -> int:
        return self._lot

    def update(self, depth: dict[str, Any], own: dict[tuple[Any, int], int], now: int) -> None:
        """A depth message ({bids, asks}: [[price, qty]] strings); own: (side, price) -> the strategy's resting qty."""
        def side(levels, s):
            out = []
            for p, q in levels:
                price = fixed(p)
                qty = fixed(q) - own.get((s, price), 0)
                if qty > 0:
                    out.append(Level(price, qty))
            return out

        self._bids = side(depth.get("bids", []), bs.Side.BUY)
        self._asks = side(depth.get("asks", []), bs.Side.SELL)
        self._data_time = now
        # Before the feed's first snapshot, and after a gap until the next one, the server's book holds nothing of
        # the market (at most the strategy's own orders): both sides present means it's live
        if self._bids and self._asks:
            self._gapped = False
            self._valid = True
        elif not self._valid or self._gapped:
            self._valid = False

    def gap(self) -> None:
        self._valid = False
        self._gapped = True
        self._bids, self._asks = [], []

    def trade(self, t: TradeEvent) -> None:
        self._last_trade = t

    def valid(self) -> bool:
        return self._valid

    def best_bid(self) -> Level | None:
        return self._bids[0] if self._valid and self._bids else None

    def best_ask(self) -> Level | None:
        return self._asks[0] if self._valid and self._asks else None

    def qty(self, side: Any, price: int) -> int:
        for lv in (self._bids if side == bs.Side.BUY else self._asks):
            if lv.price == price:
                return lv.qty
        return 0

    def top(self, side: Any, n: int) -> list[Level]:
        return list((self._bids if side == bs.Side.BUY else self._asks)[:n])

    def levels(self, side: Any) -> int:
        return len(self._bids if side == bs.Side.BUY else self._asks)

    def last_trade(self) -> TradeEvent | None:
        return self._last_trade

    def data_time(self) -> int:
        return self._data_time

    def seen_at(self) -> int:
        return self._data_time


# ---- the context ----

class LiveContext:
    """research.strategies.base.Context over the server's sockets (see the module docstring)."""

    def __init__(self, runner: LiveRunner):
        self._r = runner

    def now(self) -> int:
        return time.time_ns()

    def symbol(self) -> str:
        return self._r.symbols[0]

    def symbols(self) -> list[str]:
        return list(self._r.symbols)

    def owner(self) -> str:
        return self._r.owner

    def book(self, symbol: str | None = None) -> LiveBook:
        return self._r.books[self._r.symbol_or_primary(symbol)]

    def can_trade(self) -> bool:
        return time.time_ns() >= self._r.trading_from

    def _require_trading(self, what: str) -> None:
        if not self.can_trade():
            raise RuntimeError(f"strategy '{self._r.owner}': {what} during the warm-up; check ctx.can_trade()")

    def send(self, side: Any, type: Any, price: int, quantity: int, stp: Any = bs.StpPolicy.CANCEL_MAKER,
             symbol: str | None = None) -> str:
        self._require_trading("send")
        return self._r.send(self._r.symbol_or_primary(symbol), side, type, price, quantity, stp)

    def cancel(self, order_id: str) -> None:
        self._require_trading("cancel")
        self._r.cancel(order_id)

    def amend(self, order_id: str, new_total: int, new_price: int | None = None) -> None:
        self._require_trading("amend")
        self._r.amend(order_id, new_total, new_price)

    def replace(self, order_id: str, price: int, quantity: int) -> None:
        self._require_trading("replace")
        self._r.replace(order_id, price, quantity)

    def account(self, symbol: str | None = None) -> Account:
        return replace(self._r.accounts[self._r.symbol_or_primary(symbol)])

    def open_orders(self) -> dict[str, OpenOrder]:
        return {k: replace(v) for k, v in sorted(self._r.orders.items())}

    def set_timer(self, delay_ns: int) -> int:
        return self._r.set_timer(delay_ns)

    def cancel_timer(self, timer_id: int) -> None:
        self._r.cancelled_timers.add(timer_id)


@dataclass
class _Pending:
    action: str
    order_id: str
    symbol: str


class LiveRunner:
    def __init__(self, strategy: Any, symbols: list[str], engine: str = "http://127.0.0.1:8080", owner: str = "S1",
                 warmup_s: float = 0.0, out_dir: str | Path | None = None, cancel_on_exit: bool = True,
                 sample_ms: float = 1000.0, id_prefix: str | None = None):
        if not symbols:
            raise ValueError("at least one symbol")
        if owner in ("", "MARKET", "MARKET_TAPE"):
            raise ValueError(f"owner {owner!r} is reserved")
        self.strategy = strategy
        self.symbols = list(symbols)
        self.engine = engine.rstrip("/")
        self.owner = owner
        self.warmup_ns = int(warmup_s * 1e9)
        self.out_dir = Path(out_dir) if out_dir else None
        self.cancel_on_exit = cancel_on_exit
        self.sample_ns = int(sample_ms * 1e6)
        self.id_prefix = id_prefix if id_prefix is not None else f"{owner}-{int(time.time()) % 1_000_000}"
        self.mode = getattr(strategy, "callback_mode", "every_event")
        self.ctx = LiveContext(self)
        self.books: dict[str, LiveBook] = {}
        self.accounts = {s: Account() for s in self.symbols}
        self.orders: dict[str, OpenOrder] = {}
        self.cancelled_timers: set[int] = set()
        self.trading_from = 0
        self.stats: dict[str, int] = {"requests": 0, "reports": 0, "rejects": 0, "books": 0, "trades": 0,
                                      "timers": 0, "gaps": 0, "local_refusals": 0}
        self._seq = 0
        self._timer_seq = 0
        self._amending: set[str] = set()
        self._pending: deque[_Pending] = deque()
        self._events: asyncio.Queue | None = None
        self._outbox: asyncio.Queue | None = None
        self._tops: dict[str, tuple] = {}
        self._journal = None
        self._loop: asyncio.AbstractEventLoop | None = None

    # ---- helpers the context calls ----

    def symbol_or_primary(self, symbol: str | None) -> str:
        if symbol is None:
            return self.symbols[0]
        if symbol not in self.books:
            raise ValueError(f"unknown symbol {symbol!r}: one of {', '.join(self.symbols)}")
        return symbol

    def _write(self, kind: str, **fields: Any) -> None:
        if self._journal is not None:
            self._journal.write(json.dumps({"t": time.time_ns(), "kind": kind, **fields}, separators=(",", ":")) + "\n")

    def _request(self, payload: dict[str, Any], pending: _Pending) -> None:
        self.stats["requests"] += 1
        self._pending.append(pending)
        self._write("request", **payload)
        self._outbox.put_nowait(json.dumps(payload))

    def send(self, symbol: str, side: Any, type: Any, price: int, quantity: int, stp: Any) -> str:
        self._seq += 1
        oid = f"{self.id_prefix}-{self._seq}"
        self.orders[oid] = OpenOrder(oid, symbol, side, type, price, quantity, leaves=quantity, sent_at=time.time_ns())
        payload = {"action": "submit", "symbol": symbol, "order_id": oid, "owner": self.owner,
                   "side": side.name.lower(), "order_type": type.name.lower(), "quantity": decimal_text(quantity),
                   "stp": stp.name.lower()}
        if type != bs.OrderType.MARKET:
            payload["price"] = decimal_text(price)
        self._request(payload, _Pending("submit", oid, symbol))
        return oid

    def _mark(self, oid: str, status: Any) -> None:
        o = self.orders.get(oid)
        if o is not None:
            o.status = status
            o.sent_at = time.time_ns()

    def cancel(self, oid: str) -> None:
        o = self.orders.get(oid)
        symbol = o.symbol if o else self.symbols[0]
        self._mark(oid, bs.OrderStatus.PENDING_CANCEL)
        self._request({"action": "cancel", "symbol": symbol, "order_id": oid}, _Pending("cancel", oid, symbol))

    def amend(self, oid: str, new_total: int, new_price: int | None) -> None:
        o = self.orders.get(oid)
        symbol = o.symbol if o else self.symbols[0]
        payload = {"action": "amend", "symbol": symbol, "order_id": oid, "quantity": decimal_text(new_total)}
        if new_price is not None:
            payload["price"] = decimal_text(new_price)
        self._amending.add(oid)
        self._request(payload, _Pending("amend", oid, symbol))

    def replace(self, oid: str, price: int, quantity: int) -> None:
        o = self.orders.get(oid)
        if o is None or o.status in (bs.OrderStatus.PENDING_NEW, bs.OrderStatus.PENDING_CANCEL):
            # Not known as open: the simulator's gateway refuses it at the exchange; the server would make it a
            # new order, so it is refused here
            self.stats["local_refusals"] += 1
            self._deliver_reject(RequestReject(bs.Request.REPLACE, oid, o.symbol if o else self.symbols[0],
                                               text="Order is not open", exchange_time=time.time_ns()))
            return
        self._mark(oid, bs.OrderStatus.PENDING_REPLACE)
        payload = {"action": "replace", "symbol": o.symbol, "order_id": oid, "owner": self.owner,
                   "side": o.side.name.lower(), "order_type": o.type.name.lower(), "quantity": decimal_text(quantity),
                   "stp": "cancel_maker"}
        if o.type != bs.OrderType.MARKET:
            payload["price"] = decimal_text(price)
        self._request(payload, _Pending("replace", oid, o.symbol))

    def set_timer(self, delay_ns: int) -> int:
        self._timer_seq += 1
        tid = self._timer_seq
        self._loop.call_later(max(0, delay_ns) / 1e9, self._events.put_nowait, ("timer", tid, 0))
        return tid

    # ---- deliveries ----

    def _deliver_reject(self, rr: RequestReject) -> None:
        self.stats["rejects"] += 1
        o = self.orders.get(rr.order_id)
        if o is not None and o.status in (bs.OrderStatus.PENDING_CANCEL, bs.OrderStatus.PENDING_REPLACE):
            o.status = bs.OrderStatus.PARTIALLY_FILLED if o.filled > 0 else bs.OrderStatus.NEW
        self._write("reject", request=rr.request.name.lower(), order_id=rr.order_id, reason=rr.reason.name.lower(),
                    text=rr.text)
        self.strategy.on_reject(self.ctx, rr)

    def _on_execution(self, d: dict[str, Any]) -> None:
        if d.get("owner") != self.owner:
            return
        r = ExecReport.from_json(d)
        self._write("report", **d)
        if r.type == bs.ExecType.REJECTED and r.order_id in self._amending and r.order_id in self.orders \
                and self.orders[r.order_id].status != bs.OrderStatus.PENDING_NEW:
            # The engine refused an amend: the order is unchanged (the simulator's gateway reports it so too)
            self._amending.discard(r.order_id)
            self._deliver_reject(RequestReject(bs.Request.AMEND, r.order_id, r.symbol, r.reason, r.text, r.timestamp))
            return
        if r.type == bs.ExecType.AMENDED:
            self._amending.discard(r.order_id)
        self.stats["reports"] += 1
        o = self.orders.get(r.order_id)
        if r.type == bs.ExecType.REJECTED:
            self.orders.pop(r.order_id, None)
        elif r.type in (bs.ExecType.ACK, bs.ExecType.AMENDED):
            self.orders[r.order_id] = OpenOrder(r.order_id, r.symbol, r.side, r.order_type, r.price, r.quantity,
                                                r.cum_qty, r.leaves_qty,
                                                bs.OrderStatus.PARTIALLY_FILLED if r.cum_qty > 0 else bs.OrderStatus.NEW,
                                                o.sent_at if o else 0)
        elif r.type == bs.ExecType.FILL:
            if r.symbol in self.accounts:
                self.accounts[r.symbol].apply_fill(r)
            if o is not None:
                if r.leaves_qty == 0:
                    self.orders.pop(r.order_id, None)
                else:
                    o.filled, o.leaves = r.cum_qty, r.leaves_qty
                    if o.status in (bs.OrderStatus.NEW, bs.OrderStatus.PENDING_NEW):
                        o.status = bs.OrderStatus.PARTIALLY_FILLED
        elif r.type == bs.ExecType.CANCELLED and r.reason != bs.ExecReason.REPLACED:
            self.orders.pop(r.order_id, None)
        self.strategy.on_exec(self.ctx, r)

    def _on_order_reply(self, d: dict[str, Any]) -> None:
        if not self._pending:
            return
        p = self._pending.popleft()
        if d.get("status") != "error" or d.get("reports"):
            return  # the reports come through /ws/executions
        text = d.get("message", "")
        now = time.time_ns()
        if p.action == "cancel":
            self._deliver_reject(RequestReject(bs.Request.CANCEL, p.order_id, p.symbol, text=text, exchange_time=now))
        elif p.action == "amend":
            self._amending.discard(p.order_id)
            self._deliver_reject(RequestReject(bs.Request.AMEND, p.order_id, p.symbol, text=text, exchange_time=now))
        elif p.action == "replace":
            self._deliver_reject(RequestReject(bs.Request.REPLACE, p.order_id, p.symbol, text=text, exchange_time=now))
        else:  # a malformed order the server couldn't even parse: as a REJECTED report
            o = self.orders.pop(p.order_id, None)
            r = ExecReport(timestamp=now, type=bs.ExecType.REJECTED, text=text, order_id=p.order_id, owner_id=self.owner,
                           symbol=p.symbol, side=o.side if o else None, order_type=o.type if o else None,
                           price=o.price if o else 0, quantity=o.quantity if o else 0)
            self._write("report", order_id=p.order_id, type="rejected", text=text, local=True)
            self.strategy.on_exec(self.ctx, r)

    def _own_resting(self, symbol: str) -> dict[tuple[Any, int], int]:
        own: dict[tuple[Any, int], int] = {}
        for o in self.orders.values():
            if o.symbol == symbol and o.status != bs.OrderStatus.PENDING_NEW and o.leaves > 0:
                own[(o.side, o.price)] = own.get((o.side, o.price), 0) + o.leaves
        return own

    def _on_market_data(self, d: dict[str, Any], now: int) -> None:
        symbol = d.get("symbol")
        book = self.books.get(symbol)
        if book is None:
            return
        if d.get("event") == "gap":
            book.gap()
            self.stats["gaps"] += 1
            self._write("gap", symbol=symbol)
            self.strategy.on_gap(self.ctx)
            return
        if d.get("event") != "depth":
            return
        book.update(d.get("data", {}), self._own_resting(symbol), now)
        self.stats["books"] += 1
        if self.mode == "timer_only":
            return
        if self.mode == "on_change":
            top = (book.best_bid(), book.best_ask())
            if self._tops.get(symbol) == top:
                return
            self._tops[symbol] = top
        self.strategy.on_book(self.ctx, book)

    def _on_trade(self, d: dict[str, Any], now: int) -> None:
        t = d.get("data", {})
        symbol = d.get("symbol")
        # The market's trades are the feed's tape IOCs (synthetic IDs start with "~"); the strategy's own taker
        # trades aren't data trades
        if symbol not in self.books or not str(t.get("taker_order_id", "")).startswith("~"):
            return
        ev = TradeEvent(now, iso_ns(t.get("timestamp")), fixed(t.get("price")), fixed(t.get("quantity")),
                        enum(bs.Side, t.get("aggressor_side")), self.symbols.index(symbol))
        self.books[symbol].trade(ev)
        self.stats["trades"] += 1
        if self.mode != "timer_only":
            self.strategy.on_trade(self.ctx, ev)

    def _sample(self) -> None:
        for s in self.symbols:
            b = self.books[s]
            bid, ask = b.best_bid(), b.best_ask()
            a = self.accounts[s]
            self._write("sample", symbol=s, valid=b.valid(), bid=bid.price if bid else None, ask=ask.price if ask else None,
                        position=a.position, cash=a.cash, fees=a.fees, fills=a.fills, open_orders=sum(
                            1 for o in self.orders.values() if o.symbol == s))

    # ---- running ----

    def _specs(self) -> None:
        import requests

        for i, s in enumerate(self.symbols):
            r = requests.get(f"{self.engine}/api/symbols", params={"symbol": s}, timeout=5)
            if r.status_code != 200:
                raise ValueError(f"the server doesn't know {s} (start the feed bridge first)")
            spec = r.json()
            self.books[s] = LiveBook(s, i, fixed(spec.get("tick_size")), fixed(spec.get("lot_size")))

    def _depth_now(self, symbol: str) -> dict[str, Any]:
        import requests

        r = requests.get(f"{self.engine}/api/depth", params={"symbol": symbol}, timeout=5)
        return r.json() if r.status_code == 200 else {}

    async def run(self, duration_s: float | None = None) -> dict[str, Any]:
        import websockets

        self._loop = asyncio.get_running_loop()
        self._events = asyncio.Queue()
        self._outbox = asyncio.Queue()
        await asyncio.to_thread(self._specs)
        if self.out_dir is not None:
            self.out_dir.mkdir(parents=True, exist_ok=True)
            self._journal = open(self.out_dir / "session.jsonl", "w", encoding="utf-8")
        ws = self.engine.replace("http://", "ws://").replace("https://", "wss://")
        start = time.time_ns()
        self.trading_from = start + self.warmup_ns
        deadline = None if duration_s is None else time.monotonic() + duration_s
        async with websockets.connect(ws + "/ws/orders", max_size=None) as orders, \
                websockets.connect(ws + "/ws/executions", max_size=None) as execs, \
                websockets.connect(ws + "/ws/market-data", max_size=None) as md, \
                websockets.connect(ws + "/ws/trades", max_size=None) as trades:

            async def pump(sock, kind):
                async for frame in sock:
                    self._events.put_nowait((kind, json.loads(frame), time.time_ns()))

            async def sender():
                while True:
                    await orders.send(await self._outbox.get())

            async def sampler():
                while True:
                    await asyncio.sleep(self.sample_ns / 1e9)
                    self._events.put_nowait(("sample", None, 0))

            tasks = [asyncio.create_task(pump(orders, "reply")), asyncio.create_task(pump(execs, "exec")),
                     asyncio.create_task(pump(md, "md")), asyncio.create_task(pump(trades, "trade")),
                     asyncio.create_task(sender()), asyncio.create_task(sampler())]
            # The book as it stands now: depth is only broadcast when it changes, so a quiet market would leave the
            # strategy blind until its next change. Fetched after subscribing, so later updates are newer.
            for s in self.symbols:
                depth = await asyncio.to_thread(self._depth_now, s)
                self.books[s].update(depth, {}, time.time_ns())
            self._write("start", owner=self.owner, symbols=self.symbols, strategy=getattr(self.strategy, "name", ""))
            log.info("%s running on %s as %s", getattr(self.strategy, "name", "strategy"), ", ".join(self.symbols),
                     self.owner)
            try:
                self.strategy.on_start(self.ctx)
                while deadline is None or time.monotonic() < deadline:
                    timeout = None if deadline is None else max(0.0, deadline - time.monotonic())
                    try:
                        kind, data, now = await asyncio.wait_for(self._events.get(), timeout)
                    except asyncio.TimeoutError:
                        break
                    self._dispatch(kind, data, now)
                self.strategy.on_end(self.ctx)
                if self.cancel_on_exit:
                    await self._cancel_all()
            finally:
                for t in tasks:
                    t.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        return self._finish(start)

    def _dispatch(self, kind: str, data: Any, now: int) -> None:
        if kind == "exec":
            if data.get("event") == "execution":
                self._on_execution(data.get("data", {}))
        elif kind == "reply":
            self._on_order_reply(data)
        elif kind == "md":
            self._on_market_data(data, now)
        elif kind == "trade":
            self._on_trade(data, now)
        elif kind == "timer":
            if data in self.cancelled_timers:
                self.cancelled_timers.discard(data)
                return
            self.stats["timers"] += 1
            self.strategy.on_timer(self.ctx, data)
        elif kind == "sample":
            self._sample()

    async def _cancel_all(self, wait_s: float = 1.0) -> None:
        """At the end: cancel what's still open and take the reports for a moment."""
        for oid, o in list(self.orders.items()):
            if o.status != bs.OrderStatus.PENDING_CANCEL:
                self.cancel(oid)
        end = time.monotonic() + wait_s
        while self.orders and time.monotonic() < end:
            try:
                kind, data, now = await asyncio.wait_for(self._events.get(), max(0.0, end - time.monotonic()))
            except asyncio.TimeoutError:
                break
            if kind in ("exec", "reply"):
                self._dispatch(kind, data, now)

    def _finish(self, start: int) -> dict[str, Any]:
        self._sample()
        end = time.time_ns()
        self._write("end")
        meta = {"format": "bs_live.session/1", "owner": self.owner, "symbols": self.symbols,
                "strategy": getattr(self.strategy, "name", type(self.strategy).__name__),
                "callback_mode": self.mode, "engine": self.engine, "start_ns": start, "end_ns": end,
                "trading_ns": self.trading_from, "stats": self.stats,
                "accounts": {s: vars(a) for s, a in self.accounts.items()},
                "open_orders": len(self.orders)}
        if self._journal is not None:
            self._journal.close()
            self._journal = None
            (self.out_dir / "session_meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
        return meta


def main(argv: list[str] | None = None) -> int:
    from research.experiments.schema import make_python_strategy

    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--strategy", required=True, help="package.module:Class")
    p.add_argument("--params", default="{}", help="the strategy's parameters (JSON)")
    p.add_argument("--symbols", nargs="+", default=["BTC-USDT"], help="the engine's symbol names, the primary first")
    p.add_argument("--engine", default="http://127.0.0.1:8080")
    p.add_argument("--owner", default="S1")
    p.add_argument("--warmup", type=float, default=0.0, help="seconds before the strategy may trade")
    p.add_argument("--duration", type=float, default=None, help="seconds; until Ctrl+C without it")
    p.add_argument("--out", default=None, help="writes session.jsonl and session_meta.json there")
    p.add_argument("--keep-orders", action="store_true", help="don't cancel the strategy's orders at the end")
    args = p.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    strategy = make_python_strategy({"python": args.strategy, "params": json.loads(args.params)})
    runner = LiveRunner(strategy, args.symbols, args.engine, args.owner, args.warmup, args.out, not args.keep_orders)
    try:
        meta = asyncio.run(runner.run(args.duration))
    except KeyboardInterrupt:
        return 0
    log.info("done: %s", json.dumps({"stats": meta["stats"], "accounts": meta["accounts"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
