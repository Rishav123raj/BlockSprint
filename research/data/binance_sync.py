"""Binance's diff-depth sync rules, shared by the normalizer (P2.7, recorded data) and the live feed bridge
(P7.2, the same streams live), so the two decide every event the same way.

A ``<sym>@depth@100ms`` event carries the update IDs it covers, ``U`` (first) to ``u`` (last).

* Before a book exists (the start, or after a gap) events are buffered until a REST snapshot arrives. Events
  with ``u <= lastUpdateId`` are already in it; the first remaining one must satisfy ``U <= lastUpdateId + 1``,
  else an update between the snapshot and the buffer is missing and the next snapshot is awaited.
* Once synced, an event continues the book if ``U <= prev_u + 1 <= u``; ``u <= prev_u`` is a duplicate (two
  connections overlapping) and is dropped; anything else is a gap: the book is unknown until the next snapshot.
"""

from __future__ import annotations

from typing import Callable, Sequence, TypeVar

CONTINUE = "continue"
DUPLICATE = "duplicate"
GAP = "gap"

T = TypeVar("T")


def classify(prev_u: int, first: int, last: int) -> str:
    """What a depth event covering update IDs first..last does to a book synced up to prev_u."""
    if last <= prev_u:
        return DUPLICATE
    if first > prev_u + 1:
        return GAP
    return CONTINUE


def sync_from_snapshot(last_id: int, buffered: Sequence[T], ids: Callable[[T], tuple[int, int]]) -> tuple[bool, list[T]]:
    """A snapshot with lastUpdateId last_id against the buffered events (ids(event) = (U, u)): (True, the
    events to apply after it, in order) when it can start the book, else (False, the events still worth
    keeping for the next snapshot)."""
    pending = [e for e in buffered if ids(e)[1] > last_id]
    if pending and ids(pending[0])[0] > last_id + 1:
        return False, pending
    return True, pending
