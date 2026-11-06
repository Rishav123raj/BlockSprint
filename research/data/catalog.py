"""Catalog (P2.13): list, inspect and verify packed datasets and normalized partitions.

Every packed dataset is a directory under ``data/packed`` with one ``.bse`` file per symbol and a
``manifest.json`` (symbols, range, QC verdicts, segments, gap minutes, specs, SHA-256 of every file).

Usage::

    uv run python -m research.data.catalog list                    # packed datasets
    uv run python -m research.data.catalog list --normalized       # normalized partitions and QC verdicts
    uv run python -m research.data.catalog show <dataset>
    uv run python -m research.data.catalog verify <dataset>        # recompute every hash, check headers
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from research.data.pack import PackError, read_bse
from research.data.schema import sha256_file


def datasets(packed_root: Path | str) -> list[dict[str, Any]]:
    root = Path(packed_root)
    if not root.exists():
        return []
    out = []
    for manifest in sorted(root.glob("*/manifest.json")):
        m = json.loads(manifest.read_text(encoding="utf-8"))
        m["_dir"] = str(manifest.parent)
        out.append(m)
    return out


def normalized(norm_root: Path | str) -> list[dict[str, Any]]:
    """One row per normalized partition: symbol, date, rows, QC verdict and coverage."""
    rows = []
    for meta_path in sorted(Path(norm_root).glob("venue=*/market=*/symbol=*/date=*/meta.json")):
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        qc_path = meta_path.parent / "qc.json"
        qc = json.loads(qc_path.read_text(encoding="utf-8")) if qc_path.exists() else None
        rows.append({
            "symbol": meta["symbol"], "date": meta["date"], "rows": meta["rows"],
            "day_complete": meta["day_complete"],
            "qc_verdict": qc["verdict"] if qc else "not run",
            "book_coverage": qc["coverage"]["book_fraction"] if qc else None,
            "snapshots": f"{qc['snapshots']['matched']}/{qc['snapshots']['compared']}" if qc else "-",
            "book_sha256": meta["files"]["book.parquet"]["sha256"],
            "dir": str(meta_path.parent),
        })
    return rows


def verify(dataset_dir: Path | str) -> list[str]:
    """Problems found (empty = the dataset is intact): file hashes, sizes and .bse headers vs the manifest."""
    d = Path(dataset_dir)
    m = json.loads((d / "manifest.json").read_text(encoding="utf-8"))
    problems = []
    for entry in m["files"]:
        path = d / entry["file"]
        if not path.exists():
            problems.append(f"{entry['file']}: missing")
            continue
        if path.stat().st_size != entry["bytes"]:
            problems.append(f"{entry['file']}: {path.stat().st_size} bytes, manifest says {entry['bytes']}")
        if sha256_file(path) != entry["sha256"]:
            problems.append(f"{entry['file']}: SHA-256 differs from the manifest")
        try:
            header, records = read_bse(path)
        except PackError as e:
            problems.append(str(e))
            continue
        for key in ("record_count", "first_ts_local", "last_ts_local", "source_manifest_sha256"):
            if header[key] != entry[key]:
                problems.append(f"{entry['file']}: header {key} {header[key]} != manifest {entry[key]}")
        if header["symbol"] != entry["symbol"]:
            problems.append(f"{entry['file']}: header symbol {header['symbol']} != {entry['symbol']}")
        if header["tick_size"] != entry["spec"]["tick_size"] or header["lot_size"] != entry["spec"]["lot_size"]:
            problems.append(f"{entry['file']}: header tick/lot differ from the manifest spec")
    return problems


def _fmt_ts(ns: int) -> str:
    from datetime import datetime, timezone
    return datetime.fromtimestamp(ns / 1e9, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S") if ns else "-"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="List, show and verify market-data datasets.")
    ap.add_argument("--packed", default="data/packed")
    ap.add_argument("--norm", default="data/normalized")
    sub = ap.add_subparsers(dest="cmd", required=True)
    lst = sub.add_parser("list")
    lst.add_argument("--normalized", action="store_true", help="list normalized partitions instead")
    show = sub.add_parser("show")
    show.add_argument("dataset")
    ver = sub.add_parser("verify")
    ver.add_argument("dataset")
    args = ap.parse_args(argv)

    if args.cmd == "list" and args.normalized:
        rows = normalized(args.norm)
        if not rows:
            print(f"no normalized partitions under {args.norm}")
        for r in rows:
            cov = f"{r['book_coverage']:.2%}" if r["book_coverage"] is not None else "-"
            print(f"{r['symbol']:10s} {r['date']}  {r['qc_verdict']:8s} coverage {cov:>7s}  snapshots "
                  f"{r['snapshots']:>7s}  book {r['rows']['book']:>11,}  trades {r['rows']['trade']:>10,}  "
                  f"book sha256 {r['book_sha256'][:12]}")
        return 0
    if args.cmd == "list":
        ds = datasets(args.packed)
        if not ds:
            print(f"no datasets under {args.packed}")
        for m in ds:
            print(f"{m['dataset']}  [{m['qc_verdict']}]  {m['range']['from']} .. {m['range']['to']} "
                  f"({m['range']['days']} d)  {', '.join(m['symbols'])}")
            for e in m["files"]:
                print(f"    {e['file']:16s} {e['record_count']:>12,} records  {e['bytes'] / 1e6:9.1f} MB  "
                      f"sha256 {e['sha256']}")
        return 0

    d = Path(args.packed) / args.dataset
    if not (d / "manifest.json").exists():
        print(f"no dataset {args.dataset!r} under {args.packed}")
        return 1
    if args.cmd == "show":
        m = json.loads((d / "manifest.json").read_text(encoding="utf-8"))
        print(f"dataset {m['dataset']}  [{m['qc_verdict']}]  created {m['created_utc']}  "
              f"format {m['format']['name']} v{m['format']['version']}  schema v{m['schema_version']}")
        for e in m["files"]:
            spec = e["spec"]
            print(f"\n{e['symbol']}: {e['file']}  {e['record_count']:,} records  "
                  f"{_fmt_ts(e['first_ts_local'])} .. {_fmt_ts(e['last_ts_local'])} UTC")
            print(f"  tick {spec['tick_size_text']}  lot {spec['lot_size_text']}  kinds {e['records_by_kind']}")
            for day in e["dates"]:
                print(f"  {day['date']}  {day['qc_verdict']:8s} coverage {day['book_coverage']:.2%}  "
                      f"gap minutes {day['gap_minutes']:4d}  gaps {day['gaps']}  snapshots {day['snapshots_matched']}")
        return 0
    problems = verify(d)
    if problems:
        print(f"{args.dataset}: {len(problems)} problem(s)")
        for p in problems:
            print("  " + p)
        return 1
    print(f"{args.dataset}: OK (every file matches its manifest hash; headers consistent)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
