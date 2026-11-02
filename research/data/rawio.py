"""Raw market-data archive: append-only, hourly-rotated, zstd-compressed JSON-lines files.

Layout::

    <root>/<venue>/<market>/<SYMBOL>/<YYYY-MM-DD>/<HH>-<open_ns>.jsonl.zst

A file is written as ``*.jsonl.zst.part`` and renamed when it is closed (hour rollover or
shutdown). Data is flushed as independent zstd frames every ``flush_interval_s`` seconds, so a
``.part`` file left behind by a crash is still readable up to its last flush.

Each line is one envelope (a JSON object):

    t   local receive time, ns since the Unix epoch (taken before the payload is parsed)
    k   kind: "ws" (WebSocket frame), "snapshot" (REST depth snapshot), "event" (recorder
        lifecycle: connection open/close/error, gaps), "exchange_info", "clock" (NTP sample)
    c   connection id (ws/event), else omitted
    s   stream name for "ws" (e.g. "btcusdt@depth@100ms"), else omitted
    d   payload: the exact text received for ws/snapshot/exchange_info, or a JSON object
    t0  for "snapshot": local time the REST request was sent
"""

from __future__ import annotations

import io
import json
import os
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterator

import zstandard

PART_SUFFIX = ".part"
FILE_SUFFIX = ".jsonl.zst"


def hour_key(ts_ns: int) -> tuple[str, str]:
    """UTC (date, hour) strings for a nanosecond timestamp, e.g. ("2026-11-02", "15")."""
    dt = datetime.fromtimestamp(ts_ns / 1e9, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d"), dt.strftime("%H")


def make_envelope(ts_ns: int, kind: str, payload: Any, *, conn: int | None = None,
                  stream: str | None = None, **extra: Any) -> dict[str, Any]:
    env: dict[str, Any] = {"t": ts_ns, "k": kind}
    if conn is not None:
        env["c"] = conn
    if stream is not None:
        env["s"] = stream
    env.update(extra)
    env["d"] = payload
    return env


@dataclass
class _OpenFile:
    hour: tuple[str, str]
    part_path: Path
    handle: Any
    writer: Any
    lines: int = 0


class RawWriter:
    """Routes envelopes to one rotating archive file per symbol."""

    def __init__(self, root: Path | str, venue: str = "binance", market: str = "spot", *,
                 level: int = 3, flush_interval_s: float = 5.0,
                 clock: Callable[[], int] = time.time_ns) -> None:
        self.base = Path(root) / venue / market
        self.level = level
        self.flush_interval_ns = int(flush_interval_s * 1e9)
        self._clock = clock
        self._files: dict[str, _OpenFile] = {}
        self._last_flush_ns = clock()
        self.closed_files: list[Path] = []
        self._closed = False

    def write(self, symbol: str, envelope: dict[str, Any]) -> None:
        """Append one envelope; the file hour comes from the envelope's receive time ``t``."""
        if self._closed:
            raise RuntimeError("RawWriter is closed")
        hour = hour_key(envelope["t"])
        f = self._files.get(symbol)
        if f is None or f.hour != hour:
            if f is not None:
                self._close(symbol)
            f = self._open(symbol, hour, envelope["t"])
        line = json.dumps(envelope, separators=(",", ":"), ensure_ascii=False) + "\n"
        f.writer.write(line.encode("utf-8"))
        f.lines += 1
        self.maybe_flush()

    def maybe_flush(self) -> None:
        now = self._clock()
        if now - self._last_flush_ns >= self.flush_interval_ns:
            self.flush()

    def flush(self) -> None:
        """End the current zstd frame in every open file and push it to disk."""
        for f in self._files.values():
            f.writer.flush(zstandard.FLUSH_FRAME)
            f.handle.flush()
            os.fsync(f.handle.fileno())
        self._last_flush_ns = self._clock()

    def close(self) -> None:
        """Finalize every open file. Later writes raise instead of silently reopening files."""
        for symbol in list(self._files):
            self._close(symbol)
        self._closed = True

    def __enter__(self) -> "RawWriter":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def _open(self, symbol: str, hour: tuple[str, str], ts_ns: int) -> _OpenFile:
        directory = self.base / symbol / hour[0]
        directory.mkdir(parents=True, exist_ok=True)
        part_path = directory / f"{hour[1]}-{ts_ns}{FILE_SUFFIX}{PART_SUFFIX}"
        handle = open(part_path, "wb")
        writer = zstandard.ZstdCompressor(level=self.level).stream_writer(handle, closefd=False)
        f = _OpenFile(hour=hour, part_path=part_path, handle=handle, writer=writer)
        self._files[symbol] = f
        return f

    def _close(self, symbol: str) -> None:
        f = self._files.pop(symbol)
        f.writer.flush(zstandard.FLUSH_FRAME)
        f.writer.close()
        f.handle.flush()
        os.fsync(f.handle.fileno())
        f.handle.close()
        final = f.part_path.with_name(f.part_path.name[: -len(PART_SUFFIX)])
        f.part_path.replace(final)
        self.closed_files.append(final)


def read_raw(path: Path | str) -> Iterator[dict[str, Any]]:
    """Yield the envelopes of one archive file (``.jsonl.zst`` or a crash-truncated ``.part``).

    A ``.part`` file may end in an incomplete zstd frame or line; reading stops cleanly there.
    """
    with open(path, "rb") as fh:
        reader = zstandard.ZstdDecompressor().stream_reader(fh, read_across_frames=True)
        text = io.TextIOWrapper(reader, encoding="utf-8")
        try:
            for line in text:
                if not line.endswith("\n"):
                    return  # truncated last line of a crashed .part file
                yield json.loads(line)
        except zstandard.ZstdError:
            if str(path).endswith(PART_SUFFIX):
                return  # truncated last frame of a crashed .part file
            raise


def archive_files(root: Path | str, symbol: str, venue: str = "binance", market: str = "spot",
                  include_parts: bool = True) -> list[Path]:
    """All archive files of a symbol in time order (by hour directory and open timestamp)."""
    base = Path(root) / venue / market / symbol
    if not base.exists():
        return []
    files = [p for p in base.rglob("*")
             if p.name.endswith(FILE_SUFFIX) or (include_parts and p.name.endswith(FILE_SUFFIX + PART_SUFFIX))]

    def sort_key(p: Path) -> tuple[str, str, int]:
        hour, _, rest = p.name.partition("-")
        return p.parent.name, hour, int(rest.split(".")[0])

    return sorted(files, key=sort_key)
