"""Builders for small raw archives, written exactly as the recorder writes them (RawWriter envelopes)."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from research.data.rawio import RawWriter, make_envelope

SYMBOL = "TESTUSDT"
STREAM = SYMBOL.lower()
DAY1 = 1767225600 * 10**9  # 2026-01-01T00:00:00Z
DAY2 = DAY1 + 86_400 * 10**9
SEC = 10**9
MS = 10**6


def f8(value: str) -> str:
    """'100.01' -> '100.01000000' (Binance always sends 8 decimals)."""
    whole, _, frac = value.partition(".")
    return f"{whole}.{frac.ljust(8, '0')}"


def levels(*pairs: tuple[str, str]) -> list[list[str]]:
    return [[f8(p), f8(q)] for p, q in pairs]


class RawFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.writer = RawWriter(root, clock=lambda: 0, flush_interval_s=1e9)

    def exchange_info(self, t: int, tick: str = "0.01", lot: str = "0.001", min_notional: str = "5") -> None:
        info = {"symbols": [{"symbol": SYMBOL, "status": "TRADING", "filters": [
            {"filterType": "PRICE_FILTER", "minPrice": f8(tick), "maxPrice": "1000000.00000000", "tickSize": f8(tick)},
            {"filterType": "LOT_SIZE", "minQty": f8(lot), "maxQty": "9000.00000000", "stepSize": f8(lot)},
            {"filterType": "NOTIONAL", "minNotional": f8(min_notional)},
        ]}]}
        self.writer.write("_meta", make_envelope(t, "exchange_info", json.dumps(info)))

    def snapshot(self, t: int, last_id: int, bids: list[list[str]], asks: list[list[str]]) -> None:
        payload = json.dumps({"lastUpdateId": last_id, "bids": bids, "asks": asks})
        self.writer.write(SYMBOL, make_envelope(t, "snapshot", payload, t0=t - MS))

    def depth(self, t: int, first: int, last: int, bids: list[list[str]] = (), asks: list[list[str]] = (),
              exch_ms: int | None = None, conn: int = 1) -> None:
        data = {"e": "depthUpdate", "E": exch_ms if exch_ms is not None else t // MS, "s": SYMBOL,
                "U": first, "u": last, "b": list(bids), "a": list(asks)}
        stream = f"{STREAM}@depth@100ms"
        self.writer.write(SYMBOL, make_envelope(t, "ws", json.dumps({"stream": stream, "data": data}),
                                                conn=conn, stream=stream))

    def trade(self, t: int, tid: int, price: str, qty: str, buyer_is_maker: bool, exch_ms: int | None = None,
              conn: int = 1) -> None:
        ms = exch_ms if exch_ms is not None else t // MS
        data = {"e": "trade", "E": ms, "s": SYMBOL, "t": tid, "p": f8(price), "q": f8(qty), "T": ms,
                "m": buyer_is_maker, "M": True}
        stream = f"{STREAM}@trade"
        self.writer.write(SYMBOL, make_envelope(t, "ws", json.dumps({"stream": stream, "data": data}),
                                                conn=conn, stream=stream))

    def event(self, t: int, name: str, conn: int = 1) -> None:
        self.writer.write(SYMBOL, make_envelope(t, "event", {"event": name}, conn=conn))

    def close(self) -> None:
        self.writer.close()


@pytest.fixture
def raw(tmp_path: Path) -> RawFixture:
    return RawFixture(tmp_path / "raw")


def write_golden_stream(r: RawFixture, t0: int) -> None:
    """The stream behind the golden test, starting at t0 (see test_normalize.py for the expected rows)."""
    r.exchange_info(t0 - SEC)
    r.depth(t0 + 1 * MS, 95, 99, bids=levels(("100.00", "1")))                     # older than the snapshot
    r.depth(t0 + 2 * MS, 99, 102, bids=levels(("100.00", "2")))                    # spans lastUpdateId 100
    r.snapshot(t0 + 3 * MS, 100, levels(("100.00", "1.5"), ("99.99", "3")), levels(("100.02", "1"), ("100.03", "2")))
    r.depth(t0 + 4 * MS, 103, 103, asks=levels(("100.02", "0")))                    # removes the best ask
    r.trade(t0 + 5 * MS, 500, "100.03", "0.5", buyer_is_maker=False)                # buyer aggressor
    r.depth(t0 + 6 * MS, 104, 105, bids=levels(("100.01", "0.25")), asks=levels(("100.03", "1.5")))
