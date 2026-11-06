"""Public data downloader (P2.11): data.binance.vision daily spot files -> verified zips -> Parquet.

Datasets:

* ``trades``    every trade: id, price, qty, quote qty, time, isBuyerMaker, isBestMatch
* ``aggTrades`` aggregated trades: agg id, price, qty, first id, last id, time, isBuyerMaker, isBestMatch
* ``klines``    1-minute candles

Each zip is verified against its ``.CHECKSUM`` (SHA-256) before use; a mismatch deletes the download and
fails. Zips are cached under ``--cache`` and re-verified on every run.

Output, next to the recorded data of the same symbol and date (no book: public data has no depth):

* ``trade_public.parquet``      normalized trade schema (kind TRADE). There is no local receive time, so
                                ``ts_local`` = ``ts_exch`` = the trade time
* ``agg_trade_public.parquet``  same schema; ``seq`` = last trade id, ``seq_first`` = first trade id
* ``klines_1m.parquet``         open_time, close_time (ns), open/high/low/close (ticks), volume (lots),
                                quote_volume (1e-8 units), trades, taker_buy_volume (lots), taker_buy_quote
* ``public_meta.json``          source URLs, SHA-256s, row counts, spec

Binance switched spot timestamps from milliseconds to microseconds in 2025; the unit is detected per file.

Usage::

    uv run python -m research.data.downloader --symbols BTCUSDT --dates 2026-11-04 --datasets trades aggTrades klines
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import requests

from research.data.normalize import SpecTimeline, _date_range, strings_to_fixed
from research.data.schema import (
    ARROW_SCHEMA, COLUMNS, KIND_TRADE, NUMPY_TYPES, AGGRESSOR_BUY, AGGRESSOR_SELL, Spec, partition_dir,
)

BASE_URL = "https://data.binance.vision/data/spot/daily"
DATASETS = ("trades", "aggTrades", "klines")


class DownloadError(Exception):
    pass


def url_for(dataset: str, symbol: str, date: str, interval: str = "1m") -> str:
    if dataset == "klines":
        return f"{BASE_URL}/klines/{symbol}/{interval}/{symbol}-{interval}-{date}.zip"
    return f"{BASE_URL}/{dataset}/{symbol}/{symbol}-{dataset}-{date}.zip"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_checksum(text: str) -> str:
    """The hex digest from a .CHECKSUM file ('<sha256>  <file name>')."""
    digest = text.strip().split()[0].lower() if text.strip() else ""
    if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        raise DownloadError(f"malformed checksum file: {text[:80]!r}")
    return digest


def fetch(url: str, cache_dir: Path, session: requests.Session | None = None, timeout: float = 60) -> Path:
    """Downloads url and url.CHECKSUM into cache_dir (unless cached) and verifies the SHA-256."""
    s = session or requests.Session()
    cache_dir.mkdir(parents=True, exist_ok=True)
    name = url.rsplit("/", 1)[1]
    path = cache_dir / name
    sum_path = cache_dir / (name + ".CHECKSUM")
    if not sum_path.exists():
        r = s.get(url + ".CHECKSUM", timeout=timeout)
        if r.status_code == 404:
            raise DownloadError(f"not published (yet): {url}")
        r.raise_for_status()
        sum_path.write_text(r.text, encoding="utf-8")
    expected = parse_checksum(sum_path.read_text(encoding="utf-8"))
    if not path.exists():
        tmp = path.with_name(name + ".part")
        with s.get(url, timeout=timeout, stream=True) as r:
            if r.status_code == 404:
                raise DownloadError(f"not published (yet): {url}")
            r.raise_for_status()
            with open(tmp, "wb") as f:
                for block in r.iter_content(1 << 20):
                    f.write(block)
        os.replace(tmp, path)
    actual = sha256_bytes(path.read_bytes())
    if actual != expected:
        path.unlink()
        raise DownloadError(f"SHA-256 mismatch for {name}: expected {expected}, got {actual} (download deleted)")
    return path


def read_csv_rows(zip_path: Path) -> list[list[str]]:
    with zipfile.ZipFile(zip_path) as z:
        names = [n for n in z.namelist() if n.endswith(".csv")]
        if len(names) != 1:
            raise DownloadError(f"{zip_path.name}: expected one CSV, found {names}")
        text = io.TextIOWrapper(z.open(names[0]), encoding="utf-8")
        rows = [r for r in csv.reader(text) if r]
    if rows and not rows[0][0].lstrip("-").isdigit():  # some files carry a header row
        rows = rows[1:]
    return rows


def to_ns(times: np.ndarray) -> np.ndarray:
    """Binance timestamps (ms before 2025, µs after) -> ns."""
    if times.size == 0:
        return times
    if times.max() >= 10**15:   # microseconds
        return times * 1_000
    return times * 1_000_000    # milliseconds


def _ticks(values: list[str], step: int) -> tuple[np.ndarray, int]:
    fixed = strings_to_fixed(values)
    units, rem = np.divmod(fixed, step)
    return units, int((rem != 0).sum())


def trades_table(rows: list[list[str]], spec: Spec, aggregated: bool) -> tuple[pa.Table, int]:
    """CSV rows -> normalized trade table, and the number of off-grid values."""
    n = len(rows)
    if aggregated:  # agg id, price, qty, first id, last id, time, isBuyerMaker, isBestMatch
        seq = np.array([int(r[4]) for r in rows], dtype=np.int64)
        seq_first = np.array([int(r[3]) for r in rows], dtype=np.int64)
        t = np.array([int(r[5]) for r in rows], dtype=np.int64)
        maker = [r[6] for r in rows]
    else:           # id, price, qty, quote qty, time, isBuyerMaker, isBestMatch
        seq = np.array([int(r[0]) for r in rows], dtype=np.int64)
        seq_first = seq
        t = np.array([int(r[4]) for r in rows], dtype=np.int64)
        maker = [r[5] for r in rows]
    px, off_px = _ticks([r[1] for r in rows], spec.tick_size)
    qty, off_qty = _ticks([r[2] for r in rows], spec.lot_size)
    ts = to_ns(t)
    side = np.array([AGGRESSOR_SELL if m.lower() == "true" else AGGRESSOR_BUY for m in maker], dtype=np.uint8)
    cols = {"ts_local": ts, "ts_exch": ts, "seq": seq, "seq_first": seq_first, "seg": np.zeros(n, np.int32),
            "kind": np.full(n, KIND_TRADE, np.uint8), "side": side, "px": px, "qty": qty,
            "flags": np.zeros(n, np.uint16)}
    arrays = [pa.array(np.asarray(cols[name], dtype=NUMPY_TYPES[name]), type=typ) for name, typ in COLUMNS]
    return pa.Table.from_arrays(arrays, schema=ARROW_SCHEMA), off_px + off_qty


def klines_table(rows: list[list[str]], spec: Spec) -> pa.Table:
    """open_time, open, high, low, close, volume, close_time, quote volume, trades, taker buy base/quote."""
    def col(i: int) -> list[str]:
        return [r[i] for r in rows]
    open_time = to_ns(np.array([int(v) for v in col(0)], dtype=np.int64))
    close_time = to_ns(np.array([int(v) for v in col(6)], dtype=np.int64))
    table = {
        "open_time": open_time, "close_time": close_time,
        "open": _ticks(col(1), spec.tick_size)[0], "high": _ticks(col(2), spec.tick_size)[0],
        "low": _ticks(col(3), spec.tick_size)[0], "close": _ticks(col(4), spec.tick_size)[0],
        "volume": _ticks(col(5), spec.lot_size)[0], "quote_volume": strings_to_fixed(col(7)),
        "trades": np.array([int(v) for v in col(8)], dtype=np.int64),
        "taker_buy_volume": _ticks(col(9), spec.lot_size)[0], "taker_buy_quote": strings_to_fixed(col(10)),
    }
    return pa.table({k: pa.array(v) for k, v in table.items()})


def spec_for(symbol: str, date: str, raw_root: Path | str, session: requests.Session | None = None) -> Spec:
    """The date's spec from the recorded exchangeInfo, or else Binance's current one."""
    try:
        return SpecTimeline.from_archive(raw_root, symbol).spec_for(date)
    except ValueError:
        from research.data.meta import fetch_exchange_info
        from research.data.schema import spec_from_exchange_info
        spec = spec_from_exchange_info(fetch_exchange_info([symbol]), symbol, time.time_ns())
        if spec is None:
            raise DownloadError(f"{symbol}: not in Binance's exchangeInfo")
        return spec


def download_day(symbol: str, date: str, datasets: list[str], norm_root: Path | str, cache_root: Path | str,
                 raw_root: Path | str = "data/raw", session: requests.Session | None = None) -> dict[str, Any]:
    spec = spec_for(symbol, date, raw_root, session)
    part = partition_dir(norm_root, symbol, date)
    part.mkdir(parents=True, exist_ok=True)
    meta_path = part / "public_meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {"symbol": symbol,
                                                                                            "date": date}
    meta["spec"] = spec.to_json()
    meta.setdefault("datasets", {})
    for ds in datasets:
        url = url_for(ds, symbol, date)
        zip_path = fetch(url, Path(cache_root) / ds / symbol, session)
        rows = read_csv_rows(zip_path)
        off_grid = 0
        if ds == "klines":
            table, out_name = klines_table(rows, spec), "klines_1m.parquet"
        else:
            table, off_grid = trades_table(rows, spec, aggregated=(ds == "aggTrades"))
            out_name = "agg_trade_public.parquet" if ds == "aggTrades" else "trade_public.parquet"
        tmp = part / (out_name + ".part")
        pq.write_table(table, tmp, compression="zstd")
        os.replace(tmp, part / out_name)
        meta["datasets"][ds] = {"url": url, "zip_sha256": sha256_bytes(zip_path.read_bytes()),
                                "file": out_name, "rows": table.num_rows, "off_grid_values": off_grid,
                                "downloaded_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
    tmp = part / "public_meta.json.part"
    tmp.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    os.replace(tmp, meta_path)
    return meta


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Download and verify Binance public spot data (data.binance.vision).")
    ap.add_argument("--symbols", nargs="+", required=True)
    ap.add_argument("--dates", nargs="+")
    ap.add_argument("--from", dest="date_from")
    ap.add_argument("--to", dest="date_to")
    ap.add_argument("--datasets", nargs="+", default=["aggTrades", "klines"], choices=DATASETS)
    ap.add_argument("--out", default="data/normalized")
    ap.add_argument("--cache", default="data/downloads/binance-vision")
    ap.add_argument("--raw", default="data/raw", help="recorded archive, for the date's spec")
    args = ap.parse_args(argv)
    dates = args.dates or (_date_range(args.date_from, args.date_to) if args.date_from and args.date_to else None)
    if not dates:
        ap.error("give --dates or --from/--to")
    failed = 0
    with requests.Session() as session:
        for symbol in args.symbols:
            for date in dates:
                try:
                    meta = download_day(symbol, date, args.datasets, args.out, args.cache, args.raw, session)
                except (DownloadError, requests.RequestException) as e:
                    print(f"{symbol} {date}: FAILED {e}")
                    failed += 1
                    continue
                summary = ", ".join(f"{ds} {info['rows']:,} rows (sha256 ok)" for ds, info in meta["datasets"].items()
                                    if ds in args.datasets)
                print(f"{symbol} {date}: {summary}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
