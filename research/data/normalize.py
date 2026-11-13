"""Normalizer (P2.7, P2.8): raw recorder archive -> normalized Parquet partitions, one per symbol and UTC date.

It reads a symbol's raw files in order and keeps a local order book in sync with Binance's rules:

* Before a book exists (start, or after a gap), depth events are buffered. When a REST snapshot arrives,
  buffered events with ``u <= lastUpdateId`` are dropped and the first remaining one must satisfy
  ``U <= lastUpdateId + 1``; otherwise the snapshot is too old and the next one is awaited.
* Once synced, an event continues the book if ``U <= prev_u + 1 <= u``. Events with ``u <= prev_u`` are
  duplicates (e.g. two connections overlapping during a 24 h rollover) and are dropped. Anything else is a
  gap: a GAP row is written, a new segment starts, and the book waits for the next snapshot.
* Silence: when anything is received more than ``stale_after_s`` (default 10 s) after the last book update,
  the book stopped being live at that update (a disconnect, the PC asleep, a network stall). The GAP row is
  written then, before a date rollover could copy the stale book into the next day-start snapshot. The book
  is kept: if the next depth event continues the sequence (nothing was lost), it is re-emitted as a
  snapshot at that event's time and the new segment starts there; otherwise the next REST snapshot resyncs.
* A GAP row's time is the last good book update's (the book is unknown from then on), not the time the
  gap was noticed.

Every price and quantity is converted to ticks and lots of the spec in force on that date (from the
recorded exchangeInfo). Each date's book file starts with a snapshot of the running book at midnight
(``FLAG_SYNTHETIC_SNAPSHOT``), so one day replays on its own. REST snapshots taken while synced are not
part of the book stream; they go to ``snapshot.parquet`` so QC can check the rebuilt book against them.

``ts_local`` never decreases in the output: a row received earlier than the previous one (system clock
stepped back, or a depth event buffered until its sync snapshot arrived) takes the previous row's time and
gets ``FLAG_TS_CLAMPED``.

Usage::

    uv run python -m research.data.normalize --raw data/raw --out data/normalized --symbols BTCUSDT ETHUSDT
    uv run python -m research.data.normalize --symbols BTCUSDT --dates 2026-11-04 2026-11-05
"""

from __future__ import annotations

import argparse
import json
import os
import time
from array import array
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from itertools import repeat
from operator import itemgetter
from pathlib import Path
from typing import Any, Iterable

import numpy as np
import orjson
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from research.data.binance_sync import CONTINUE, DUPLICATE, GAP, classify, sync_from_snapshot
from research.data.rawio import FILE_SUFFIX, PART_SUFFIX, archive_files, read_raw
from research.data.schema import (
    AGGRESSOR_BUY, AGGRESSOR_SELL, ARROW_SCHEMA, COLUMNS, FLAG_DUPLICATE_SUSPECT, FLAG_FROM_SNAPSHOT, FLAG_LAST_IN_BATCH, FLAG_OFF_GRID,
    FLAG_SYNTHETIC_SNAPSHOT, FLAG_TS_CLAMPED, KIND_DELTA, KIND_GAP, KIND_SNAP_BEGIN, KIND_SNAP_END,
    KIND_SNAP_LEVEL, KIND_TRADE, NUMPY_TYPES, SCHEMA_VERSION, SIDE_ASK, SIDE_BID, Spec, parse_fixed8,
    partition_dir, sha256_file, spec_from_exchange_info,
)

NORMALIZER_VERSION = 1
META_SYMBOL = "_meta"
DAY_NS = 86_400 * 10**9
_TYPECODES = {"int64": "q", "int32": "i", "uint8": "B", "uint16": "H"}
_FIRST = itemgetter(0)
_SECOND = itemgetter(1)
_EIGHT_DECIMALS = r"^[0-9]+\.[0-9]{8}$"


def strings_to_fixed(values: list[str]) -> np.ndarray:
    """Decimal strings -> int64 array in 1e-8 units, in bulk (Arrow kernels). Binance sends exactly 8
    decimals; anything else goes through the exact scalar parser."""
    if not values:
        return np.empty(0, dtype=np.int64)
    arr = pa.array(values, pa.string())
    if pc.all(pc.match_substring_regex(arr, _EIGHT_DECIMALS)).as_py():
        return pc.cast(pc.replace_substring(arr, ".", ""), pa.int64()).to_numpy()
    return np.fromiter((parse_fixed8(v) for v in values), dtype=np.int64, count=len(values))


def date_of(ts_ns: int) -> str:
    return datetime.fromtimestamp(ts_ns // 10**9, tz=timezone.utc).strftime("%Y-%m-%d")


def day_start_ns(date: str) -> int:
    return int(datetime.strptime(date, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp()) * 10**9


# ---------------------------------------------------------------------------------------------------------
# Specs over time
# ---------------------------------------------------------------------------------------------------------

class SpecTimeline:
    """The specs of one symbol over time, from the exchangeInfo envelopes in the archive's _meta files."""

    def __init__(self, symbol: str, specs: list[Spec]) -> None:
        self.symbol = symbol
        self.specs = sorted(specs, key=lambda s: s.source_ts)
        if not self.specs:
            raise ValueError(f"no exchangeInfo for {symbol} in the archive: can't convert to ticks and lots")

    @staticmethod
    def from_archive(raw_root: Path | str, symbol: str) -> "SpecTimeline":
        specs = []
        for path in archive_files(raw_root, META_SYMBOL):
            for env in read_raw(path):
                if env["k"] == "exchange_info":
                    spec = spec_from_exchange_info(env["d"], symbol, env["t"])
                    if spec is not None:
                        specs.append(spec)
        return SpecTimeline(symbol, specs)

    def spec_for(self, date: str) -> Spec:
        """The spec in force at the start of the date; else the first one fetched that day; else the first."""
        start = day_start_ns(date)
        before = [s for s in self.specs if s.source_ts <= start]
        if before:
            return before[-1]
        return self.specs[0]


# ---------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------

class TableSink:
    """Column buffers for one Parquet file, written in row groups of up to chunk_rows (zstd)."""

    def __init__(self, path: Path, chunk_rows: int) -> None:
        self.path = path
        self.chunk_rows = chunk_rows
        self.cols = {name: array(_TYPECODES[NUMPY_TYPES[name]]) for name, _ in COLUMNS}
        self.writer: pq.ParquetWriter | None = None
        self.rows = 0
        self._part = path.with_name(path.name + PART_SUFFIX)

    def add(self, ts_local: int, ts_exch: int, seq: int, seq_first: int, seg: int, kind: int,
            sides: list[int], pxs: list[int], qtys: list[int], flags: list[int]) -> None:
        n = len(pxs)
        c = self.cols
        c["ts_local"].extend(repeat(ts_local, n))
        c["ts_exch"].extend(repeat(ts_exch, n))
        c["seq"].extend(repeat(seq, n))
        c["seq_first"].extend(repeat(seq_first, n))
        c["seg"].extend(repeat(seg, n))
        c["kind"].extend(repeat(kind, n))
        c["side"].extend(sides)
        c["px"].extend(pxs)
        c["qty"].extend(qtys)
        c["flags"].extend(flags)
        self.rows += n
        if len(c["px"]) >= self.chunk_rows:
            self.flush()

    def add_one(self, ts_local: int, ts_exch: int, seq: int, seq_first: int, seg: int, kind: int, side: int,
                px: int, qty: int, flags: int) -> None:
        c = self.cols
        c["ts_local"].append(ts_local)
        c["ts_exch"].append(ts_exch)
        c["seq"].append(seq)
        c["seq_first"].append(seq_first)
        c["seg"].append(seg)
        c["kind"].append(kind)
        c["side"].append(side)
        c["px"].append(px)
        c["qty"].append(qty)
        c["flags"].append(flags)
        self.rows += 1
        if len(c["px"]) >= self.chunk_rows:
            self.flush()

    def add_arrays(self, **columns: np.ndarray) -> None:
        """Appends whole columns (numpy arrays of one length, named as in the schema)."""
        c = self.cols
        for name, _ in COLUMNS:
            c[name].frombytes(np.ascontiguousarray(columns[name], dtype=NUMPY_TYPES[name]).tobytes())
        self.rows += len(columns["px"])
        if len(c["px"]) >= self.chunk_rows:
            self.flush()

    def _table(self) -> pa.Table:
        arrays = [pa.array(np.frombuffer(self.cols[name], dtype=NUMPY_TYPES[name]) if len(self.cols[name])
                           else np.empty(0, dtype=NUMPY_TYPES[name]), type=typ) for name, typ in COLUMNS]
        return pa.Table.from_arrays(arrays, schema=ARROW_SCHEMA)

    def flush(self) -> None:
        if not len(self.cols["px"]):
            return
        if self.writer is None:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.writer = pq.ParquetWriter(self._part, ARROW_SCHEMA, compression="zstd")
        table = self._table()
        self.writer.write_table(table, row_group_size=table.num_rows)
        for name, _ in COLUMNS:
            self.cols[name] = array(_TYPECODES[NUMPY_TYPES[name]])

    def close(self) -> None:
        self.flush()
        if self.writer is None:  # an empty file, so readers always find all three
            self.path.parent.mkdir(parents=True, exist_ok=True)
            pq.write_table(self._table(), self._part, compression="zstd")
        else:
            self.writer.close()
        os.replace(self._part, self.path)


@dataclass
class DateStats:
    depth_events: int = 0
    depth_duplicates_dropped: int = 0
    duplicate_suspect_events: int = 0
    gaps: int = 0
    sync_attempts_failed: int = 0
    snapshots_received: int = 0
    trades: int = 0
    trade_duplicates_dropped: int = 0
    trade_id_gaps: int = 0
    trades_missing: int = 0
    ts_clamped_rows: int = 0
    off_grid_rows: int = 0
    synced_ns: int = 0
    first_ts: int | None = None
    last_ts: int | None = None
    segments: dict[int, dict[str, Any]] = field(default_factory=dict)


class DateOutput:
    """The three Parquet files and meta.json of one symbol-date partition."""

    def __init__(self, out_root: Path, symbol: str, date: str, spec: Spec, chunk_rows: int) -> None:
        self.dir = partition_dir(out_root, symbol, date)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.symbol, self.date, self.spec = symbol, date, spec
        self.book = TableSink(self.dir / "book.parquet", chunk_rows)
        self.trade = TableSink(self.dir / "trade.parquet", chunk_rows)
        self.snapshot = TableSink(self.dir / "snapshot.parquet", chunk_rows)
        self.stats = DateStats()
        self.sources: dict[str, int] = {}

    def close(self, input_end_ts: int, input_includes_part_files: bool) -> dict[str, Any]:
        for sink in (self.book, self.trade, self.snapshot):
            sink.close()
        s = self.stats
        end = day_start_ns(self.date) + DAY_NS
        meta = {
            "schema_version": SCHEMA_VERSION,
            "normalizer_version": NORMALIZER_VERSION,
            "symbol": self.symbol,
            "date": self.date,
            "spec": self.spec.to_json(),
            "rows": {"book": self.book.rows, "trade": self.trade.rows, "snapshot": self.snapshot.rows},
            "first_ts_local": s.first_ts,
            "last_ts_local": s.last_ts,
            "day_complete": input_end_ts >= end,
            "input_end_ts": input_end_ts,
            "input_includes_part_files": input_includes_part_files,
            "synced_seconds": round(s.synced_ns / 1e9, 3),
            "stats": {k: v for k, v in vars(s).items() if k not in ("segments", "first_ts", "last_ts", "synced_ns")},
            "segments": [s.segments[k] for k in sorted(s.segments)],
            "sources": [{"file": name, "bytes": size} for name, size in sorted(self.sources.items())],
            "files": {name: {"sha256": sha256_file(self.dir / name), "bytes": (self.dir / name).stat().st_size}
                      for name in ("book.parquet", "trade.parquet", "snapshot.parquet")},
            "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        tmp = self.dir / "meta.json.part"
        tmp.write_text(json.dumps(meta, indent=2), encoding="utf-8")
        os.replace(tmp, self.dir / "meta.json")
        return meta


# ---------------------------------------------------------------------------------------------------------
# Normalizer
# ---------------------------------------------------------------------------------------------------------

class Normalizer:
    """One symbol's raw envelopes in, normalized partitions out.

    Sequence decisions (duplicate, continue, gap) are made per event in Python; the accepted events' levels
    are queued and converted in bulk (``_flush``), which is where the volume is (~40 M book rows per day for
    BTCUSDT). Anything that needs the current book (a date rollover, a sync snapshot, a gap) flushes first.
    """

    def __init__(self, symbol: str, out_root: Path | str, timeline: SpecTimeline,
                 dates: Iterable[str] | None = None, chunk_rows: int = 500_000,
                 buffer_limit: int = 100_000, batch_rows: int = 200_000, stale_after_s: float = 10.0) -> None:
        self.symbol = symbol
        self.out_root = Path(out_root)
        self.timeline = timeline
        self.dates = set(dates) if dates is not None else None
        self.chunk_rows = chunk_rows
        self.buffer_limit = buffer_limit
        self.batch_rows = batch_rows
        self.stale_ns = int(stale_after_s * 1e9)

        # Book state, fixed-point 1e-8 units (independent of the date's spec). A removed level stays as a
        # 0 entry until the next snapshot of the book prunes it: dict.update can then apply whole batches.
        self.bids: dict[int, int] = {}
        self.asks: dict[int, int] = {}
        self.synced = False
        self.prev_u: int | None = None
        self.seg = 0
        self.buffer: list[tuple[int, dict]] = []
        self.just_synced = False  # the next event is the first after a sync (it may start before L + 1)
        self.last_book_ts: int | None = None
        self.last_book_exch = 0
        self.suspended = False    # the book went silent: kept, not live, until continued or resynced
        self.last_trade_id: int | None = None

        # Queued depth rows: level strings plus one tuple per event (ts, ts_exch, u, U, flags, rows)
        self.q_px: list[str] = []
        self.q_qty: list[str] = []
        self.q_side = array("B")
        self.q_events: list[tuple[int, int, int, int, int, int]] = []
        # Queued trades: (ts, ts_exch, id, aggressor side, flags) plus price and quantity strings
        self.t_rows: list[tuple[int, int, int, int, int]] = []
        self.t_px: list[str] = []
        self.t_qty: list[str] = []

        # Output state
        self.last_ts = 0
        self.out: DateOutput | None = None
        self.out_date: str | None = None
        self.day_start = 0
        self.day_end = -1
        self.tick = 1
        self.lot = 1
        self.metas: list[dict[str, Any]] = []
        self.input_end_ts = 0
        self.saw_part_files = False
        self.current_file: tuple[str, int] | None = None

    # -- driving -----------------------------------------------------------------------------------------

    def run(self, files: list[Path]) -> list[dict[str, Any]]:
        for path in files:
            self.current_file = (path.name, path.stat().st_size)
            self.saw_part_files |= path.name.endswith(PART_SUFFIX)
            for env in read_raw(path, loads=orjson.loads):
                self.feed(env)
        self.finish()
        return self.metas

    def feed(self, env: dict[str, Any]) -> None:
        kind = env["k"]
        t = env["t"]
        if t > self.input_end_ts:
            self.input_end_ts = t
        if self.synced and self.last_book_ts is not None and t - self.last_book_ts > self.stale_ns:
            self._silence()
        if kind == "ws":
            stream = env["s"]
            data = orjson.loads(env["d"])["data"]
            if stream.endswith("@trade"):
                self._trade(t, data)
            elif "@depth" in stream:
                self._depth(t, data)
        elif kind == "snapshot":
            self._snapshot(t, orjson.loads(env["d"]))

    def finish(self) -> None:
        self._close_date()

    # -- time and partitions -----------------------------------------------------------------------------

    def _stamp(self, t: int) -> tuple[int, int]:
        """(output ts_local, extra flags): never earlier than the previous row."""
        if t < self.last_ts:
            return self.last_ts, FLAG_TS_CLAMPED
        self.last_ts = t
        return t, 0

    def _route(self, ts: int) -> DateOutput | None:
        """The output for a row at ts, rolling over to a new date (with its day-start snapshot)."""
        if ts >= self.day_end:
            self._close_date()
            date = date_of(ts)
            self.out_date = date
            self.day_start = day_start_ns(date)
            self.day_end = self.day_start + DAY_NS
            spec = self.timeline.spec_for(date)
            self.tick, self.lot = spec.tick_size, spec.lot_size
            if self.dates is None or date in self.dates:
                self.out = DateOutput(self.out_root, self.symbol, date, spec, self.chunk_rows)
                if self.synced:
                    self._emit_book_snapshot(self.out.book, self.day_start, self.prev_u or 0,
                                             FLAG_FROM_SNAPSHOT | FLAG_SYNTHETIC_SNAPSHOT, self.bids, self.asks)
                    self._segment(self.out).setdefault("synced_from_ts", self.day_start)
            else:
                self.out = None
        out = self.out
        if out is not None:
            st = out.stats
            if st.first_ts is None:
                st.first_ts = ts
            st.last_ts = ts
            if self.current_file is not None:
                out.sources[self.current_file[0]] = self.current_file[1]
        return out

    def _close_date(self) -> None:
        self._flush()
        if self.out is not None:
            meta = self.out.close(self.input_end_ts, self.saw_part_files)
            self.metas.append(meta)
            self.out = None

    def _segment(self, out: DateOutput) -> dict[str, Any]:
        return out.stats.segments.setdefault(self.seg, {"seg": self.seg})

    # -- bulk conversion ---------------------------------------------------------------------------------

    def _flush(self) -> None:
        self._flush_depth()
        self._flush_trades()

    def _flush_depth(self) -> None:
        if not self.q_events:
            return
        px = strings_to_fixed(self.q_px)
        qty = strings_to_fixed(self.q_qty)
        side = np.frombuffer(self.q_side, dtype=np.uint8)
        is_bid = side == SIDE_BID
        self.bids.update(zip(px[is_bid].tolist(), qty[is_bid].tolist()))
        self.asks.update(zip(px[~is_bid].tolist(), qty[~is_bid].tolist()))
        out = self.out
        if out is not None:
            ev = np.array(self.q_events, dtype=np.int64)
            counts = ev[:, 5]
            px_t, px_r = np.divmod(px, self.tick)
            qty_t, qty_r = np.divmod(qty, self.lot)
            off = (px_r != 0) | (qty_r != 0)
            flags = np.repeat(ev[:, 4], counts).astype(np.uint16)
            flags[np.cumsum(counts) - 1] |= FLAG_LAST_IN_BATCH
            flags[off] |= FLAG_OFF_GRID
            out.book.add_arrays(
                ts_local=np.repeat(ev[:, 0], counts), ts_exch=np.repeat(ev[:, 1], counts),
                seq=np.repeat(ev[:, 2], counts), seq_first=np.repeat(ev[:, 3], counts),
                seg=np.full(len(px), self.seg, dtype=np.int32), kind=np.full(len(px), KIND_DELTA, dtype=np.uint8),
                side=side, px=px_t, qty=qty_t, flags=flags)
            st = out.stats
            st.off_grid_rows += int(off.sum())
            st.ts_clamped_rows += int(counts[(ev[:, 4] & FLAG_TS_CLAMPED) != 0].sum())
        self.q_px, self.q_qty, self.q_side, self.q_events = [], [], array("B"), []

    def _flush_trades(self) -> None:
        if not self.t_rows:
            return
        out = self.out
        if out is not None:
            rows = np.array(self.t_rows, dtype=np.int64)
            px_t, px_r = np.divmod(strings_to_fixed(self.t_px), self.tick)
            qty_t, qty_r = np.divmod(strings_to_fixed(self.t_qty), self.lot)
            off = (px_r != 0) | (qty_r != 0)
            flags = rows[:, 4].astype(np.uint16)
            flags[off] |= FLAG_OFF_GRID
            n = len(rows)
            out.trade.add_arrays(
                ts_local=rows[:, 0], ts_exch=rows[:, 1], seq=rows[:, 2], seq_first=rows[:, 2],
                seg=np.full(n, self.seg, dtype=np.int32), kind=np.full(n, KIND_TRADE, dtype=np.uint8),
                side=rows[:, 3].astype(np.uint8), px=px_t, qty=qty_t, flags=flags)
            st = out.stats
            st.off_grid_rows += int(off.sum())
            st.ts_clamped_rows += int(((rows[:, 4] & FLAG_TS_CLAMPED) != 0).sum())
        self.t_rows, self.t_px, self.t_qty = [], [], []

    def _emit_book_snapshot(self, sink: TableSink, ts: int, seq: int, flags: int,
                            bids: dict[int, int], asks: dict[int, int]) -> int:
        """SNAP_BEGIN, SNAP_LEVEL rows (bids best first, then asks best first), SNAP_END. Prunes 0 levels."""
        for book in (bids, asks):
            for p in [p for p, q in book.items() if not q]:
                del book[p]
        sink.add_one(ts, 0, seq, seq, self.seg, KIND_SNAP_BEGIN, 0, 0, 0, flags)
        off = 0
        for side, book, ordered in ((SIDE_BID, bids, sorted(bids, reverse=True)), (SIDE_ASK, asks, sorted(asks))):
            n = len(ordered)
            if not n:
                continue
            px = np.array(ordered, dtype=np.int64)
            qty = np.array([book[p] for p in ordered], dtype=np.int64)
            px_t, px_r = np.divmod(px, self.tick)
            qty_t, qty_r = np.divmod(qty, self.lot)
            bad = (px_r != 0) | (qty_r != 0)
            fl = np.full(n, flags, dtype=np.uint16)
            fl[bad] |= FLAG_OFF_GRID
            off += int(bad.sum())
            sink.add_arrays(ts_local=np.full(n, ts, dtype=np.int64), ts_exch=np.zeros(n, dtype=np.int64),
                            seq=np.full(n, seq, dtype=np.int64), seq_first=np.full(n, seq, dtype=np.int64),
                            seg=np.full(n, self.seg, dtype=np.int32),
                            kind=np.full(n, KIND_SNAP_LEVEL, dtype=np.uint8),
                            side=np.full(n, side, dtype=np.uint8), px=px_t, qty=qty_t, flags=fl)
        sink.add_one(ts, 0, seq, seq, self.seg, KIND_SNAP_END, 0, 0, 0, flags | FLAG_LAST_IN_BATCH)
        return off

    # -- events ------------------------------------------------------------------------------------------

    def _snapshot(self, t: int, snap: dict) -> None:
        last_id = snap["lastUpdateId"]
        bids = dict(zip(strings_to_fixed([b[0] for b in snap["bids"]]).tolist(),
                        strings_to_fixed([b[1] for b in snap["bids"]]).tolist()))
        asks = dict(zip(strings_to_fixed([a[0] for a in snap["asks"]]).tolist(),
                        strings_to_fixed([a[1] for a in snap["asks"]]).tolist()))
        ts, clamp = self._stamp(t)
        out = self._route(ts)
        if out is not None:
            out.stats.snapshots_received += 1
            out.stats.off_grid_rows += self._emit_book_snapshot(out.snapshot, ts, last_id, FLAG_FROM_SNAPSHOT | clamp,
                                                                bids, asks)
            if clamp:
                out.stats.ts_clamped_rows += len(bids) + len(asks) + 2
        if not self.synced:
            self._try_sync(ts, last_id, bids, asks, clamp)

    def _try_sync(self, ts: int, last_id: int, bids: dict[int, int], asks: dict[int, int], clamp: int) -> None:
        ok, pending = sync_from_snapshot(last_id, self.buffer, lambda e: (e[1]["U"], e[1]["u"]))
        if not ok:
            # The snapshot is older than the oldest buffered event: an update between them is missing
            self.buffer = pending
            if self.out is not None:
                self.out.stats.sync_attempts_failed += 1
            return
        self._flush()
        out = self.out
        if self.suspended:
            self.suspended = False
            if out is not None and self.prev_u is not None:
                self._segment(out).setdefault("missed_updates", max(0, last_id - self.prev_u))
        self.bids, self.asks = dict(bids), dict(asks)
        self.synced = True
        self.just_synced = True
        self.prev_u = last_id
        self.buffer = []
        self.last_book_ts = ts
        self.last_book_exch = 0
        if out is not None:
            out.stats.off_grid_rows += self._emit_book_snapshot(out.book, ts, last_id, FLAG_FROM_SNAPSHOT | clamp,
                                                                self.bids, self.asks)
            self._segment(out).setdefault("synced_from_ts", ts)
        for t, data in pending:
            self._apply(t, data)

    def _depth(self, t: int, data: dict) -> None:
        if self.synced:
            self._apply(t, data)
            return
        if self.suspended:
            if classify(self.prev_u, data["U"], data["u"]) != GAP:
                if classify(self.prev_u, data["U"], data["u"]) == CONTINUE:
                    self._resume(t)
                self._apply(t, data)  # a duplicate is counted and dropped there
                return
            # Updates were lost while silent: the GAP row is already written
            self.suspended = False
            if self.out is not None:
                self._segment(self.out)["missed_updates"] = data["U"] - self.prev_u - 1
            self.bids, self.asks = {}, {}
        self.buffer.append((t, data))
        if len(self.buffer) > self.buffer_limit:
            del self.buffer[: len(self.buffer) - self.buffer_limit]

    def _apply(self, t: int, data: dict) -> None:
        first, last = data["U"], data["u"]
        prev = self.prev_u
        what = classify(prev, first, last)
        if what == DUPLICATE:
            ts, _ = self._stamp(t)
            out = self._route(ts)
            if out is not None:
                out.stats.depth_duplicates_dropped += 1
            return
        if what == GAP:
            self._gap(t, data)
            return
        ts, clamp = self._stamp(t)
        out = self._route(ts)
        # Overlapping updates already applied: expected for the first event after a sync (Binance's rule
        # U <= lastUpdateId + 1 <= u), suspicious later (e.g. two connections during a handover)
        overlap = first <= prev and not self.just_synced
        self.just_synced = False
        b, a = data["b"], data["a"]
        n = len(b) + len(a)
        if n:
            self.q_px.extend(map(_FIRST, b))
            self.q_px.extend(map(_FIRST, a))
            self.q_qty.extend(map(_SECOND, b))
            self.q_qty.extend(map(_SECOND, a))
            self.q_side.extend(repeat(SIDE_BID, len(b)))
            self.q_side.extend(repeat(SIDE_ASK, len(a)))
            self.q_events.append((ts, data["E"] * 1_000_000, last, first,
                                  clamp | (FLAG_DUPLICATE_SUSPECT if overlap else 0), n))
        self.prev_u = last
        self.last_book_exch = data["E"] * 1_000_000
        if out is not None:
            st = out.stats
            st.depth_events += 1
            if overlap:
                st.duplicate_suspect_events += 1
            if self.last_book_ts is not None and ts > self.last_book_ts:
                st.synced_ns += ts - max(self.last_book_ts, self.day_start)
            seg = self._segment(out)
            seg.setdefault("first_ts", ts)
            seg["last_ts"] = ts
        self.last_book_ts = ts
        if len(self.q_px) >= self.batch_rows:
            self._flush_depth()

    def _gap(self, t: int, data: dict) -> None:
        self._emit_gap(data["U"], data["U"] - self.prev_u - 1)
        self.synced = False
        self.bids, self.asks = {}, {}
        self.buffer = [(t, data)]

    def _silence(self) -> None:
        self._emit_gap(self.prev_u + 1, None)
        self.synced = False
        self.suspended = True  # the book and prev_u are kept for _resume

    def _emit_gap(self, seq_first: int, missed: int | None) -> None:
        """GAP row at the last good book update, in that update's date; starts a new segment."""
        self._flush()  # queued rows belong to the old segment
        ts = max(self.last_book_ts, self.day_start)  # after a day-start snapshot taken in between
        self.seg += 1
        out = self.out
        if out is not None:
            out.stats.gaps += 1
            out.book.add_one(ts, self.last_book_exch, self.prev_u, seq_first, self.seg, KIND_GAP, 0, 0, 0,
                             FLAG_LAST_IN_BATCH)
            seg = self._segment(out)
            seg["gap_ts"] = ts
            if missed is not None:
                seg["missed_updates"] = missed
        self.last_book_ts = None

    def _resume(self, t: int) -> None:
        """The depth stream continues after a silence with nothing lost: the kept book is exact again."""
        ts, clamp = self._stamp(t)
        out = self._route(ts)
        self._flush()
        self.synced = True
        self.suspended = False
        self.just_synced = False
        self.last_book_ts = ts
        if out is not None:
            out.stats.off_grid_rows += self._emit_book_snapshot(
                out.book, ts, self.prev_u, FLAG_FROM_SNAPSHOT | FLAG_SYNTHETIC_SNAPSHOT | clamp, self.bids, self.asks)
            seg = self._segment(out)
            seg.setdefault("synced_from_ts", ts)
            seg.setdefault("missed_updates", 0)

    def _trade(self, t: int, data: dict) -> None:
        tid = data["t"]
        last = self.last_trade_id
        ts, clamp = self._stamp(t)
        out = self._route(ts)
        if last is not None and tid <= last:
            if out is not None:
                out.stats.trade_duplicates_dropped += 1
            return
        self.last_trade_id = tid
        if out is None:
            return
        st = out.stats
        if last is not None and tid > last + 1:
            st.trade_id_gaps += 1
            st.trades_missing += tid - last - 1
        st.trades += 1
        self.t_rows.append((ts, data["T"] * 1_000_000, tid, AGGRESSOR_SELL if data["m"] else AGGRESSOR_BUY, clamp))
        self.t_px.append(data["p"])
        self.t_qty.append(data["q"])
        if len(self.t_rows) >= self.batch_rows:
            self._flush_trades()


# ---------------------------------------------------------------------------------------------------------
# Entry points
# ---------------------------------------------------------------------------------------------------------

def archive_dates(raw_root: Path | str, symbol: str) -> list[str]:
    return sorted({p.parent.name for p in archive_files(raw_root, symbol)})


def files_for(raw_root: Path | str, symbol: str, dates: list[str] | None, lookback_hours: float) -> list[Path]:
    """The symbol's raw files covering the dates, starting lookback_hours earlier (to sync the book)."""
    files = archive_files(raw_root, symbol)
    if dates is None:
        return files
    start = day_start_ns(min(dates)) - int(lookback_hours * 3600) * 10**9
    end = day_start_ns(max(dates)) + DAY_NS

    def file_hour_ns(p: Path) -> int:
        return day_start_ns(p.parent.name) + int(p.name.split("-", 1)[0]) * 3600 * 10**9

    return [p for p in files if start - 3600 * 10**9 < file_hour_ns(p) < end]


def normalize_symbol(raw_root: Path | str, out_root: Path | str, symbol: str, dates: list[str] | None = None,
                     lookback_hours: float = 2.0, chunk_rows: int = 500_000,
                     stale_after_s: float = 10.0) -> list[dict[str, Any]]:
    timeline = SpecTimeline.from_archive(raw_root, symbol)
    files = files_for(raw_root, symbol, dates, lookback_hours)
    return Normalizer(symbol, out_root, timeline, dates, chunk_rows, stale_after_s=stale_after_s).run(files)


def _date_range(start: str, end: str) -> list[str]:
    d0 = datetime.strptime(start, "%Y-%m-%d")
    d1 = datetime.strptime(end, "%Y-%m-%d")
    return [(d0 + timedelta(days=i)).strftime("%Y-%m-%d") for i in range((d1 - d0).days + 1)]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Normalize the raw recorder archive into Parquet partitions.")
    ap.add_argument("--raw", default="data/raw", help="raw archive root (default data/raw)")
    ap.add_argument("--out", default="data/normalized", help="output root (default data/normalized)")
    ap.add_argument("--symbols", nargs="+", required=True)
    ap.add_argument("--dates", nargs="+", help="UTC dates YYYY-MM-DD (default: every date in the archive)")
    ap.add_argument("--from", dest="date_from", help="first UTC date (with --to)")
    ap.add_argument("--to", dest="date_to", help="last UTC date (with --from)")
    ap.add_argument("--lookback-hours", type=float, default=2.0,
                    help="raw data read before the first date to sync the book (default 2)")
    ap.add_argument("--stale-after-s", type=float, default=10.0,
                    help="no book update for longer than this is a gap (default 10)")
    args = ap.parse_args(argv)

    dates = args.dates
    if args.date_from or args.date_to:
        if not (args.date_from and args.date_to):
            ap.error("--from and --to go together")
        dates = _date_range(args.date_from, args.date_to)

    for symbol in args.symbols:
        t0 = time.perf_counter()
        metas = normalize_symbol(args.raw, args.out, symbol, dates, args.lookback_hours,
                                 stale_after_s=args.stale_after_s)
        print(f"{symbol}: {len(metas)} date(s) in {time.perf_counter() - t0:.1f} s")
        for m in metas:
            st = m["stats"]
            print(f"  {m['date']}  book {m['rows']['book']:>10,}  trades {m['rows']['trade']:>9,}  "
                  f"snapshots {st['snapshots_received']:>4}  gaps {st['gaps']:>2}  "
                  f"synced {m['synced_seconds'] / 3600:5.2f} h  "
                  f"{'complete' if m['day_complete'] else 'partial'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
