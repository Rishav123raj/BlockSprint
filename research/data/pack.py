"""Packer (P2.12): normalized partitions -> one ``.bse`` replay file per symbol, plus the dataset manifest.

The book and trade streams of each date are merged by ``(ts_local, source, position)``: receive time,
then book (0) before trade (1) at equal times, then the original order within each stream (which keeps a
depth event's rows, and a snapshot's rows, together). Dates are concatenated in order; each starts with
its day-start snapshot, so the replayer can also start at any date boundary.

``.bse`` format v1 (all integers little-endian; see the 2.0 plan, section 14.2):

    header, 512 bytes
        0   8  magic "BSEVENT\\0"          8  4  format version (1)       12  4  record size (40)
        16  8  record count                24  8  first ts_local           32  8  last ts_local
        40 32  SHA-256 of the source manifest (the normalized files packed, canonical JSON)
        72 32  symbol (ASCII, zero-padded)
        104 8  tick size (1e-8 units)      112 8  lot size (1e-8 units)
        120 1  price decimals              121 1  qty decimals            122 8  min notional (1e-8 units)
        130 .. reserved (zero)
    record, 40 bytes
        0 ts_local i64 | 8 ts_exch i64 | 16 px i64 (ticks) | 24 qty i64 (lots) | 32 seq u32 (low 32 bits)
        36 kind u8 | 37 side u8 | 38 flags u16

The record has no segment column: a GAP record starts a new segment, as in the normalized data.

Usage::

    uv run python -m research.data.pack pack --symbols BTCUSDT ETHUSDT --from 2026-11-05 --to 2026-11-07
    uv run python -m research.data.pack pack --symbols BTCUSDT --from 2026-11-05 --to 2026-11-05 --allow-partial
    uv run python -m research.data.pack info data/packed/<dataset>/BTCUSDT.bse
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator

import numpy as np
import pyarrow.parquet as pq

from research.data.normalize import _date_range
from research.data.schema import (
    KIND_NAMES, SCHEMA_VERSION, Spec, partition_dir, sha256_file,
)

MAGIC = b"BSEVENT\0"
FORMAT_VERSION = 1
HEADER_SIZE = 512
RECORD_DTYPE = np.dtype([("ts_local", "<i8"), ("ts_exch", "<i8"), ("px", "<i8"), ("qty", "<i8"),
                         ("seq", "<u4"), ("kind", "u1"), ("side", "u1"), ("flags", "<u2")])
RECORD_SIZE = RECORD_DTYPE.itemsize  # 40
_HEADER = struct.Struct("<8sIIQqq32s32sqqBBq")  # up to offset 130
_COLS = ["ts_local", "ts_exch", "seq", "kind", "side", "px", "qty", "flags"]
assert RECORD_SIZE == 40 and _HEADER.size == 130


class PackError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------------------------------------

def read_header(path: Path | str) -> dict[str, Any]:
    with open(path, "rb") as f:
        raw = f.read(HEADER_SIZE)
    if len(raw) < HEADER_SIZE:
        raise PackError(f"{path}: shorter than the {HEADER_SIZE}-byte header")
    (magic, version, record_size, count, first_ts, last_ts, manifest_sha, symbol, tick, lot, px_dec, qty_dec,
     min_notional) = _HEADER.unpack_from(raw)
    if magic != MAGIC:
        raise PackError(f"{path}: not a .bse file (magic {magic!r})")
    if version != FORMAT_VERSION or record_size != RECORD_SIZE:
        raise PackError(f"{path}: unsupported format version {version} / record size {record_size}")
    if raw[_HEADER.size:].strip(b"\0"):
        raise PackError(f"{path}: reserved header bytes are not zero")
    return {"format_version": version, "record_size": record_size, "record_count": count,
            "first_ts_local": first_ts, "last_ts_local": last_ts, "source_manifest_sha256": manifest_sha.hex(),
            "symbol": symbol.rstrip(b"\0").decode("ascii"), "tick_size": tick, "lot_size": lot,
            "price_decimals": px_dec, "qty_decimals": qty_dec, "min_notional": min_notional}


def read_bse(path: Path | str) -> tuple[dict[str, Any], np.memmap]:
    """(header, records) with the records memory-mapped as a structured array."""
    header = read_header(path)
    size = os.path.getsize(path)
    expected = HEADER_SIZE + header["record_count"] * RECORD_SIZE
    if size != expected:
        raise PackError(f"{path}: {size} bytes, expected {expected} for {header['record_count']} records")
    if header["record_count"] == 0:
        return header, np.zeros(0, dtype=RECORD_DTYPE)
    records = np.memmap(path, dtype=RECORD_DTYPE, mode="r", offset=HEADER_SIZE, shape=(header["record_count"],))
    return header, records


# ---------------------------------------------------------------------------------------------------------
# Merging
# ---------------------------------------------------------------------------------------------------------

def _to_records(cols: dict[str, np.ndarray]) -> np.ndarray:
    out = np.empty(len(cols["ts_local"]), dtype=RECORD_DTYPE)
    for name in ("ts_local", "ts_exch", "px", "qty", "kind", "side", "flags"):
        out[name] = cols[name]
    out["seq"] = (cols["seq"] & 0xFFFFFFFF).astype(np.uint32)
    return out


def _merge(book: np.ndarray, trades: np.ndarray) -> np.ndarray:
    """Stable merge by (ts_local, source): book rows first at equal times, each stream in its own order."""
    both = np.concatenate([book, trades])
    source = np.concatenate([np.zeros(len(book), np.uint8), np.ones(len(trades), np.uint8)])
    order = np.lexsort((source, both["ts_local"]))  # lexsort is stable: original order breaks remaining ties
    return both[order]


def merged_records(part: Path, batch_size: int = 1_000_000) -> Iterator[np.ndarray]:
    """One date's book and trade rows merged, in chunks of records."""
    tcols = pq.read_table(part / "trade.parquet", columns=_COLS)
    trades = _to_records({c: tcols.column(c).to_numpy() for c in _COLS})
    ti = 0
    pf = pq.ParquetFile(part / "book.parquet")
    for batch in pf.iter_batches(batch_size=batch_size, columns=_COLS):
        book = _to_records({c: batch.column(c).to_numpy() for c in _COLS})
        if not len(book):
            continue
        # Trades strictly before the batch's last time; equal-time ones wait (the book may continue)
        end = ti + int(np.searchsorted(trades["ts_local"][ti:], book["ts_local"][-1], side="left"))
        yield _merge(book, trades[ti:end])
        ti = end
    if ti < len(trades):
        yield trades[ti:]


# ---------------------------------------------------------------------------------------------------------
# Packing
# ---------------------------------------------------------------------------------------------------------

def _header_bytes(symbol: str, spec: Spec, count: int, first_ts: int, last_ts: int, manifest_sha: bytes) -> bytes:
    raw = _HEADER.pack(MAGIC, FORMAT_VERSION, RECORD_SIZE, count, first_ts, last_ts, manifest_sha,
                       symbol.encode("ascii").ljust(32, b"\0")[:32], spec.tick_size, spec.lot_size,
                       spec.price_decimals, spec.qty_decimals, spec.min_notional)
    return raw.ljust(HEADER_SIZE, b"\0")


def pack_symbol(norm_root: Path | str, out_dir: Path, symbol: str, dates: list[str],
                allow_partial: bool = False) -> dict[str, Any]:
    """Packs one symbol's dates into out_dir/<SYMBOL>.bse; returns its manifest entry."""
    parts, metas, qcs = [], [], []
    for date in dates:
        part = partition_dir(norm_root, symbol, date)
        if not (part / "meta.json").exists():
            raise PackError(f"{symbol} {date}: not normalized ({part})")
        if not (part / "qc.json").exists():
            raise PackError(f"{symbol} {date}: no QC report; run research.data.qc first")
        meta = json.loads((part / "meta.json").read_text(encoding="utf-8"))
        qc = json.loads((part / "qc.json").read_text(encoding="utf-8"))
        if qc["verdict"] == "fail":
            raise PackError(f"{symbol} {date}: QC failed ({qc['checks']})")
        if qc["verdict"] != "pass" and not allow_partial:
            raise PackError(f"{symbol} {date}: QC verdict {qc['verdict']!r} "
                            f"(coverage {qc['coverage']['book_fraction']:.2%}); pass --allow-partial to pack it")
        for name, info in meta["files"].items():
            if sha256_file(part / name) != info["sha256"]:
                raise PackError(f"{symbol} {date}: {name} changed since it was normalized (SHA-256 mismatch)")
        parts.append(part)
        metas.append(meta)
        qcs.append(qc)

    spec = Spec.from_json(metas[0]["spec"])
    for meta in metas[1:]:
        other = Spec.from_json(meta["spec"])
        if not spec.same_rules(other):
            raise PackError(f"{symbol}: trading rules change on {meta['date']} (tick/lot "
                            f"{other.tick_size}/{other.lot_size} vs {spec.tick_size}/{spec.lot_size}); "
                            "prices are in ticks, so pack the periods separately")

    sources = [{"date": m["date"], "file": f"{name}", "sha256": m["files"][name]["sha256"]}
               for m in metas for name in ("book.parquet", "trade.parquet")]
    source_manifest = json.dumps({"symbol": symbol, "schema_version": SCHEMA_VERSION, "sources": sources},
                                 sort_keys=True, separators=(",", ":")).encode()
    source_sha = hashlib.sha256(source_manifest).digest()

    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{symbol}.bse"
    tmp = path.with_name(path.name + ".part")
    count = 0
    first_ts = last_ts = 0
    kinds = np.zeros(256, dtype=np.int64)
    prev_last = None
    with open(tmp, "wb") as f:
        f.write(b"\0" * HEADER_SIZE)
        for part in parts:
            for chunk in merged_records(part):
                if not len(chunk):
                    continue
                if prev_last is not None and chunk["ts_local"][0] < prev_last:
                    raise PackError(f"{part}: ts_local goes backwards across chunks or dates")
                if count == 0:
                    first_ts = int(chunk["ts_local"][0])
                prev_last = last_ts = int(chunk["ts_local"][-1])
                kinds += np.bincount(chunk["kind"], minlength=256)
                f.write(chunk.tobytes())
                count += len(chunk)
        f.seek(0)
        f.write(_header_bytes(symbol, spec, count, first_ts, last_ts, source_sha))
    os.replace(tmp, path)

    return {
        "symbol": symbol,
        "file": path.name,
        "sha256": sha256_file(path),
        "bytes": path.stat().st_size,
        "record_count": count,
        "records_by_kind": {KIND_NAMES[k]: int(kinds[k]) for k in KIND_NAMES if kinds[k]},
        "first_ts_local": first_ts,
        "last_ts_local": last_ts,
        "source_manifest_sha256": source_sha.hex(),
        "sources": sources,
        "spec": spec.to_json(),
        "dates": [{
            "date": m["date"],
            "qc_verdict": q["verdict"],
            "book_coverage": q["coverage"]["book_fraction"],
            "gap_minutes": 1440 - q["coverage"]["book_minutes"],
            "gaps": q["gaps"]["gap_rows"],
            "snapshots_matched": f"{q['snapshots']['matched']}/{q['snapshots']['compared']}",
            "segments": m["segments"],
            "rows": m["rows"],
            "day_complete": m["day_complete"],
        } for m, q in zip(metas, qcs)],
    }


def pack_dataset(norm_root: Path | str, packed_root: Path | str, symbols: list[str], dates: list[str],
                 name: str | None = None, allow_partial: bool = False) -> Path:
    name = name or f"binance_spot_{'-'.join(symbols)}_{dates[0]}_{dates[-1]}"
    out_dir = Path(packed_root) / name
    entries = [pack_symbol(norm_root, out_dir, s, dates, allow_partial) for s in symbols]
    verdicts = [d["qc_verdict"] for e in entries for d in e["dates"]]
    manifest = {
        "dataset": name,
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "format": {"name": "bse", "version": FORMAT_VERSION, "record_size": RECORD_SIZE},
        "schema_version": SCHEMA_VERSION,
        "venue": "binance", "market": "spot",
        "symbols": symbols,
        "range": {"from": dates[0], "to": dates[-1], "days": len(dates)},
        "qc_verdict": "pass" if all(v == "pass" for v in verdicts) else "partial",
        "files": entries,
    }
    tmp = out_dir / "manifest.json.part"
    tmp.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    os.replace(tmp, out_dir / "manifest.json")
    return out_dir


# ---------------------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------------------

def _info(path: str, show: int) -> None:
    header, records = read_bse(path)
    print(json.dumps(header, indent=2))
    if len(records):
        kinds = np.bincount(records["kind"], minlength=6)
        print("records by kind:", {KIND_NAMES[k]: int(kinds[k]) for k in KIND_NAMES if kinds[k]})
        print("ts_local non-decreasing:", bool((np.diff(records["ts_local"]) >= 0).all()))
    for r in records[:show]:
        print({name: int(r[name]) for name in RECORD_DTYPE.names} | {"kind_name": KIND_NAMES[int(r["kind"])]})


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Pack normalized partitions into .bse replay files.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack", help="pack a date range into a dataset")
    p.add_argument("--norm", default="data/normalized")
    p.add_argument("--out", default="data/packed")
    p.add_argument("--symbols", nargs="+", required=True)
    p.add_argument("--from", dest="date_from", required=True)
    p.add_argument("--to", dest="date_to", required=True)
    p.add_argument("--name", help="dataset name (default binance_spot_<symbols>_<from>_<to>)")
    p.add_argument("--allow-partial", action="store_true", help="also pack dates whose QC verdict is 'partial'")
    i = sub.add_parser("info", help="print a .bse file's header and first records")
    i.add_argument("file")
    i.add_argument("--records", type=int, default=5)
    args = ap.parse_args(argv)

    if args.cmd == "info":
        _info(args.file, args.records)
        return 0
    try:
        out = pack_dataset(args.norm, args.out, args.symbols, _date_range(args.date_from, args.date_to),
                           args.name, args.allow_partial)
    except PackError as e:
        print(f"error: {e}")
        return 1
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    print(f"dataset {manifest['dataset']} ({manifest['qc_verdict']}) -> {out}")
    for e in manifest["files"]:
        print(f"  {e['file']}: {e['record_count']:,} records, {e['bytes'] / 1e6:.1f} MB, sha256 {e['sha256'][:16]}...")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
