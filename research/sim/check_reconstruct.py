"""Validation gate V0: the C++ reconstruction (``bs_sim reconstruct``) must equal the Python oracle.

``bs_sim reconstruct FILE.bse --every K --top N --out dump.csv`` writes the top-N levels per side at
checkpoints, numbered by book batches (depth events, snapshots and gaps). This script replays the same
dates' normalized ``book.parquet`` through ``research.data.refbook.RefBook``, counting book batches the
same way (rows flagged LAST_IN_BATCH), and compares every checkpoint exactly.

QC already matched the oracle to Binance's REST snapshots, so a pass here ties the C++ book to Binance's
own snapshots too.

Usage::

    build/Release/bs_sim reconstruct data/packed/<dataset>/BTCUSDT.bse --every 20000 --top 20 --out v0.csv
    uv run python -m research.sim.check_reconstruct --dump v0.csv --symbol BTCUSDT --dates 2026-11-05
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

from research.data.refbook import RefBook, iter_rows
from research.data.schema import FLAG_LAST_IN_BATCH, partition_dir


def load_dump(path: Path) -> dict[int, dict]:
    """batch index -> {"valid": bool, "bids": [(px, qty)...], "asks": [...]} (duplicates must agree)."""
    checkpoints: dict[int, dict] = {}
    rows_by_key: dict[tuple[int, int], list] = defaultdict(list)  # (batch, ts) -> rows
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows_by_key[(int(r["batch"]), int(r["ts_local"]))].append(r)
    for (batch, _), rows in rows_by_key.items():
        if rows[0]["valid"] == "0":
            cp = {"valid": False, "bids": [], "asks": []}
        else:
            cp = {"valid": True,
                  "bids": [(int(r["px"]), int(r["qty"])) for r in rows if r["side"] == "0"],
                  "asks": [(int(r["px"]), int(r["qty"])) for r in rows if r["side"] == "1"]}
        if batch in checkpoints and checkpoints[batch] != cp:
            raise SystemExit(f"dump: checkpoints for batch {batch} disagree with each other")
        checkpoints[batch] = cp
    return checkpoints


def check(dump: Path, norm_root: Path, symbol: str, dates: list[str], top_n: int) -> int:
    checkpoints = load_dump(dump)
    wanted = sorted(checkpoints)
    book = RefBook()
    batches = 0
    compared = matched = 0
    mismatches: list[str] = []
    wi = 0

    def compare(batch: int) -> None:
        nonlocal compared, matched
        cp = checkpoints[batch]
        compared += 1
        if not book.valid:
            ok = not cp["valid"]
            got = {"valid": False}
        else:
            top = book.top(top_n)
            ok = cp["valid"] and top["bids"] == cp["bids"] and top["asks"] == cp["asks"]
            got = top
        if ok:
            matched += 1
        elif len(mismatches) < 5:
            mismatches.append(f"batch {batch}: C++ {cp}\n    oracle {got}")

    while wi < len(wanted) and wanted[wi] == 0:  # a checkpoint before any book batch
        compare(0)
        wi += 1
    for date in dates:
        path = partition_dir(norm_root, symbol, date) / "book.parquet"
        for row in iter_rows(path):
            book.apply(row)
            if row.flags & FLAG_LAST_IN_BATCH:
                batches += 1
                while wi < len(wanted) and wanted[wi] == batches:
                    compare(batches)
                    wi += 1
    missing = len(wanted) - wi
    print(f"{symbol} {', '.join(dates)}: {batches:,} book batches in the oracle; "
          f"{compared} checkpoint(s) compared, {matched} identical (top {top_n} per side)")
    if missing:
        print(f"  {missing} checkpoint(s) beyond the oracle's last batch (dump and data don't match)")
    for m in mismatches:
        print("  MISMATCH " + m)
    ok = compared > 0 and matched == compared and missing == 0
    print("V0 PASS" if ok else "V0 FAIL")
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="V0: compare bs_sim reconstruct checkpoints with the Python oracle.")
    ap.add_argument("--dump", required=True, type=Path)
    ap.add_argument("--norm", default="data/normalized", type=Path)
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--dates", nargs="+", required=True, help="the dates packed into the .bse file, in order")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args(argv)
    return check(args.dump, args.norm, args.symbol, args.dates, args.top)


if __name__ == "__main__":
    sys.exit(main())
