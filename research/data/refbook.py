"""Python reference book (P2.9): the oracle for normalized book events.

Deliberately simple and slow: one dict per side, price (ticks) -> quantity (lots). It applies normalized
rows exactly as the schema defines them and answers "what did the book look like" at any point:

* SNAP_BEGIN clears the book; SNAP_LEVEL sets a level; SNAP_END marks it valid
* DELTA sets a level's absolute quantity (0 removes it)
* GAP clears the book and marks it invalid until the next snapshot

The C++ simulator's reconstruction mode (P3.5) must match it on every snapshot (validation gate V0).

Usage::

    from research.data.refbook import RefBook, iter_rows
    book = RefBook()
    for row in iter_rows("data/normalized/.../date=2026-11-05/book.parquet"):
        book.apply(row)
    print(book.top(5))
"""

from __future__ import annotations

import heapq
from pathlib import Path
from typing import Iterator, NamedTuple

import pyarrow.parquet as pq

from research.data.schema import (
    KIND_DELTA, KIND_GAP, KIND_SNAP_BEGIN, KIND_SNAP_END, KIND_SNAP_LEVEL, SIDE_ASK, SIDE_BID,
)


class Row(NamedTuple):
    ts_local: int
    ts_exch: int
    seq: int
    seq_first: int
    seg: int
    kind: int
    side: int
    px: int
    qty: int
    flags: int


def iter_rows(path: Path | str, batch_size: int = 262_144) -> Iterator[Row]:
    """Rows of a normalized Parquet file, in file order."""
    pf = pq.ParquetFile(path)
    for batch in pf.iter_batches(batch_size=batch_size, columns=list(Row._fields)):
        cols = [batch.column(name).to_numpy(zero_copy_only=False).tolist() for name in Row._fields]
        yield from map(Row._make, zip(*cols))


class RefBook:
    """Levels in dicts; the best prices come from heaps with lazy deletion (a popped price is simply
    skipped if its level is gone), so best_bid / best_ask are cheap enough to call after every event."""

    def __init__(self) -> None:
        self.bids: dict[int, int] = {}
        self.asks: dict[int, int] = {}
        self._bid_heap: list[int] = []  # negated prices
        self._ask_heap: list[int] = []
        self.valid = False      # a snapshot has been applied since the start or the last gap
        self.in_snapshot = False
        self.seg = 0
        self.last_seq = 0

    def _set(self, side: int, px: int, qty: int) -> None:
        if side == SIDE_BID:
            if qty:
                if px not in self.bids:
                    heapq.heappush(self._bid_heap, -px)
                self.bids[px] = qty
            else:
                self.bids.pop(px, None)
        else:
            if qty:
                if px not in self.asks:
                    heapq.heappush(self._ask_heap, px)
                self.asks[px] = qty
            else:
                self.asks.pop(px, None)

    def clear(self) -> None:
        self.bids.clear()
        self.asks.clear()
        self._bid_heap.clear()
        self._ask_heap.clear()

    def apply(self, row: Row | tuple) -> None:
        """Applies one row (a Row or a plain tuple in schema column order)."""
        kind = row[5]
        if kind == KIND_DELTA:
            self._set(row[6], row[7], row[8])
            self.last_seq = row[2]
        elif kind == KIND_SNAP_LEVEL:
            self._set(row[6], row[7], row[8])
        elif kind == KIND_SNAP_BEGIN:
            self.clear()
            self.in_snapshot = True
            self.valid = False
            self.seg = row[4]
        elif kind == KIND_SNAP_END:
            self.in_snapshot = False
            self.valid = True
            self.last_seq = row[2]
        elif kind == KIND_GAP:
            self.clear()
            self.valid = False
            self.seg = row[4]

    def best_bid(self) -> tuple[int, int] | None:
        heap = self._bid_heap
        while heap and -heap[0] not in self.bids:
            heapq.heappop(heap)
        return (-heap[0], self.bids[-heap[0]]) if heap else None

    def best_ask(self) -> tuple[int, int] | None:
        heap = self._ask_heap
        while heap and heap[0] not in self.asks:
            heapq.heappop(heap)
        return (heap[0], self.asks[heap[0]]) if heap else None

    def top(self, n: int) -> dict[str, list[tuple[int, int]]]:
        """The best n levels per side, best first, as (px, qty)."""
        bids = sorted(self.bids.items(), key=lambda kv: -kv[0])[:n]
        asks = sorted(self.asks.items())[:n]
        return {"bids": bids, "asks": asks}

    def is_crossed(self) -> bool:
        """Best bid at or above best ask (a locked book counts)."""
        bid, ask = self.best_bid(), self.best_ask()
        return bid is not None and ask is not None and bid[0] >= ask[0]
