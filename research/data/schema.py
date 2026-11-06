"""Normalized market-data schema shared by the normalizer, QC, packer and catalog (P2.7-P2.13).

One row per book level change, snapshot level, trade or gap marker:

    ts_local   int64   recorder receive time, ns (the simulation drives on this). Never decreases
                       within a partition; see FLAG_TS_CLAMPED
    ts_exch    int64   exchange time, ns (depth: event time E; trade: trade time T; snapshot: 0)
    seq        int64   depth: final update ID u; snapshot: lastUpdateId; trade: trade ID; gap: last u
                       applied before the gap
    seq_first  int64   depth: first update ID U; otherwise equal to seq (gap: U of the event that
                       revealed it)
    seg        int32   book segment; a new one starts at every GAP row
    kind       uint8   KIND_*
    side       uint8   book: 0 bid / 1 ask; trade: aggressor 0 buy / 1 sell
    px         int64   price in ticks of the date's spec
    qty        int64   DELTA: new absolute level quantity in lots (0 = level removed);
                       SNAP_LEVEL: level quantity; TRADE: traded lots
    flags      uint16  FLAG_* bits

Partitions: <root>/venue=binance/market=spot/symbol=<SYMBOL>/date=<YYYY-MM-DD>/ with
book.parquet, trade.parquet, snapshot.parquet and meta.json.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass
from pathlib import Path

import pyarrow as pa

SCHEMA_VERSION = 1

KIND_SNAP_BEGIN = 0
KIND_SNAP_LEVEL = 1
KIND_SNAP_END = 2
KIND_DELTA = 3
KIND_TRADE = 4
KIND_GAP = 5
KIND_NAMES = {KIND_SNAP_BEGIN: "SNAP_BEGIN", KIND_SNAP_LEVEL: "SNAP_LEVEL", KIND_SNAP_END: "SNAP_END",
              KIND_DELTA: "DELTA", KIND_TRADE: "TRADE", KIND_GAP: "GAP"}

SIDE_BID = 0
SIDE_ASK = 1
AGGRESSOR_BUY = 0
AGGRESSOR_SELL = 1

FLAG_LAST_IN_BATCH = 1 << 0        # last row of one depth event, snapshot or synthetic snapshot
FLAG_FROM_SNAPSHOT = 1 << 1        # row comes from a REST snapshot
FLAG_DUPLICATE_SUSPECT = 1 << 2    # depth event overlapping updates already applied (connection handover)
FLAG_TS_CLAMPED = 1 << 3           # ts_local raised to keep it non-decreasing (clock step back, or a depth
                                   # event buffered until the sync snapshot arrived)
FLAG_SYNTHETIC_SNAPSHOT = 1 << 4   # day-start snapshot built from the running book, not from REST
FLAG_OFF_GRID = 1 << 5             # price or quantity not a multiple of the date's tick / lot size

COLUMNS: list[tuple[str, pa.DataType]] = [
    ("ts_local", pa.int64()),
    ("ts_exch", pa.int64()),
    ("seq", pa.int64()),
    ("seq_first", pa.int64()),
    ("seg", pa.int32()),
    ("kind", pa.uint8()),
    ("side", pa.uint8()),
    ("px", pa.int64()),
    ("qty", pa.int64()),
    ("flags", pa.uint16()),
]
NUMPY_TYPES = {"ts_local": "int64", "ts_exch": "int64", "seq": "int64", "seq_first": "int64", "seg": "int32",
               "kind": "uint8", "side": "uint8", "px": "int64", "qty": "int64", "flags": "uint16"}
ARROW_SCHEMA = pa.schema(COLUMNS)

SCALE = 10**8  # fixed-point unit: 1e-8, Binance's finest precision (same as the C++ engine)

VENUE = "binance"
MARKET = "spot"


def parse_fixed8(text: str) -> int:
    """Exact decimal string -> integer in 1e-8 units. Fast path for Binance's 8-decimal strings."""
    dot = text.find(".")
    if dot == -1:
        return int(text) * SCALE
    decimals = len(text) - dot - 1
    if decimals == 8:
        return int(text[:dot] + text[dot + 1:])
    if decimals > 8:
        if text[dot + 9:].strip("0"):
            raise ValueError(f"{text!r} is finer than 1e-8")
        return int(text[:dot] + text[dot + 1:dot + 9])
    return int(text[:dot] + text[dot + 1:] + "0" * (8 - decimals))


def format_fixed8(value: int) -> str:
    sign = "-" if value < 0 else ""
    whole, frac = divmod(abs(value), SCALE)
    frac_text = f"{frac:08d}".rstrip("0") or "0"
    return f"{sign}{whole}.{frac_text}"


def decimals_of(step: int) -> int:
    """Number of decimals a step in 1e-8 units needs: 0.01 -> 2, 0.00001 -> 5, 1 -> 0."""
    if step <= 0:
        return 8
    d = 8
    while d > 0 and step % 10 == 0:
        step //= 10
        d -= 1
    return d


@dataclass(frozen=True)
class Spec:
    """A symbol's trading rules in 1e-8 units, from Binance's exchangeInfo filters."""

    symbol: str
    tick_size: int
    lot_size: int
    min_qty: int = 0
    max_qty: int = 0
    min_notional: int = 0
    source_ts: int = 0  # receive time of the exchangeInfo it came from (0 = given by hand)

    @property
    def price_decimals(self) -> int:
        return decimals_of(self.tick_size)

    @property
    def qty_decimals(self) -> int:
        return decimals_of(self.lot_size)

    def same_rules(self, other: "Spec") -> bool:
        return (self.tick_size, self.lot_size, self.min_qty, self.max_qty, self.min_notional) == (
            other.tick_size, other.lot_size, other.min_qty, other.max_qty, other.min_notional)

    def to_json(self) -> dict:
        d = asdict(self)
        d["tick_size_text"] = format_fixed8(self.tick_size)
        d["lot_size_text"] = format_fixed8(self.lot_size)
        return d

    @staticmethod
    def from_json(d: dict) -> "Spec":
        return Spec(d["symbol"], d["tick_size"], d["lot_size"], d.get("min_qty", 0), d.get("max_qty", 0),
                    d.get("min_notional", 0), d.get("source_ts", 0))


def spec_from_exchange_info(text: str, symbol: str, source_ts: int = 0) -> Spec | None:
    """The symbol's Spec from an exchangeInfo response, or None if the symbol isn't in it."""
    for s in json.loads(text)["symbols"]:
        if s["symbol"] != symbol:
            continue
        filters = {f["filterType"]: f for f in s["filters"]}
        price = filters.get("PRICE_FILTER", {})
        lot = filters.get("LOT_SIZE", {})
        notional = filters.get("NOTIONAL") or filters.get("MIN_NOTIONAL") or {}
        return Spec(symbol=symbol,
                    tick_size=parse_fixed8(price.get("tickSize", "0.00000001")),
                    lot_size=parse_fixed8(lot.get("stepSize", "0.00000001")),
                    min_qty=parse_fixed8(lot.get("minQty", "0")),
                    max_qty=parse_fixed8(lot.get("maxQty", "0")),
                    min_notional=parse_fixed8(notional.get("minNotional", "0")),
                    source_ts=source_ts)
    return None


def partition_dir(root: Path | str, symbol: str, date: str, venue: str = VENUE, market: str = MARKET) -> Path:
    return Path(root) / f"venue={venue}" / f"market={market}" / f"symbol={symbol}" / f"date={date}"


def sha256_file(path: Path | str, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while block := f.read(chunk):
            h.update(block)
    return h.hexdigest()
