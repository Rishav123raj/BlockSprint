"""Reference book (P2.9), QC (P2.10), packer (P2.12) and catalog (P2.13)."""

import json

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from conftest import DAY1, DAY2, MS, SEC, SYMBOL, levels, write_golden_stream

from research.data import catalog
from research.data.normalize import normalize_symbol
from research.data.pack import PackError, pack_dataset, read_bse
from research.data.qc import run_qc
from research.data.refbook import RefBook, Row, iter_rows
from research.data.schema import (
    KIND_DELTA, KIND_GAP, KIND_SNAP_BEGIN, KIND_SNAP_END, KIND_SNAP_LEVEL, KIND_TRADE, partition_dir,
)

T0 = DAY1 + 3600 * SEC
DATE = "2026-01-01"


def normalized(raw, tmp_path):
    raw.close()
    out = tmp_path / "norm"
    normalize_symbol(raw.root, out, SYMBOL)
    return out


def part(out, date=DATE):
    return partition_dir(out, SYMBOL, date)


# -- reference book ------------------------------------------------------------------------------------------

def row(kind, side=0, px=0, qty=0, seq=1, seg=0):
    return Row(0, 0, seq, seq, seg, kind, side, px, qty, 0)


def test_refbook_applies_snapshots_deltas_and_gaps():
    b = RefBook()
    for r in (row(KIND_SNAP_BEGIN), row(KIND_SNAP_LEVEL, 0, 100, 5), row(KIND_SNAP_LEVEL, 1, 102, 7),
              row(KIND_SNAP_END, seq=10)):
        b.apply(r)
    assert b.valid and b.best_bid() == (100, 5) and b.best_ask() == (102, 7)
    b.apply(row(KIND_DELTA, 0, 101, 2, seq=11))
    b.apply(row(KIND_DELTA, 0, 101, 0, seq=12))   # removed ...
    b.apply(row(KIND_DELTA, 0, 101, 3, seq=13))   # ... and re-added: the lazy heap must not lose it
    assert b.best_bid() == (101, 3)
    b.apply(row(KIND_DELTA, 1, 102, 0, seq=14))
    assert b.best_ask() is None
    assert b.top(2) == {"bids": [(101, 3), (100, 5)], "asks": []}
    b.apply(row(KIND_GAP, seg=1))
    assert not b.valid and b.best_bid() is None and b.seg == 1


def test_refbook_rebuilds_the_golden_book(raw, tmp_path):
    write_golden_stream(raw, T0)
    out = normalized(raw, tmp_path)
    b = RefBook()
    for r in iter_rows(part(out) / "book.parquet"):
        b.apply(r)
    assert b.top(5) == {"bids": [(10001, 250), (10000, 2000), (9999, 3000)], "asks": [(10003, 1500)]}


# -- QC ---------------------------------------------------------------------------------------------------------

def golden_with_periodic_snapshot(raw, bid_qty="0.25"):
    write_golden_stream(raw, T0)
    raw.depth(T0 + 20 * MS, 106, 106, asks=levels(("100.04", "1")))
    # Taken while synced, at update 105: the state after the event 104..105
    raw.snapshot(T0 + 30 * MS, 105, levels(("100.01", bid_qty), ("100.00", "2"), ("99.99", "3")),
                 levels(("100.03", "1.5")))
    raw.depth(T0 + 40 * MS, 107, 107, bids=levels(("99.98", "1")))


def test_qc_matches_periodic_snapshots_and_reports_partial_day(raw, tmp_path):
    golden_with_periodic_snapshot(raw)
    out = normalized(raw, tmp_path)
    qc = run_qc(part(out))
    assert qc["snapshots"]["compared"] == 2 and qc["snapshots"]["matched"] == 2  # sync + periodic
    assert qc["checks"] == {"snapshots_match": True, "never_crossed": True, "on_grid": True,
                            "ts_local_monotonic": True}
    assert qc["verdict"] == "partial"  # 1 minute of a day
    assert (part(out) / "qc.html").read_text(encoding="utf-8").startswith("<!doctype html>")
    assert json.loads((part(out) / "qc.json").read_text())["verdict"] == "partial"


def test_qc_fails_when_the_rebuilt_book_disagrees_with_a_snapshot(raw, tmp_path):
    golden_with_periodic_snapshot(raw, bid_qty="0.3")  # the exchange says 0.3 where the stream says 0.25
    out = normalized(raw, tmp_path)
    qc = run_qc(part(out))
    assert qc["verdict"] == "fail" and not qc["checks"]["snapshots_match"]
    assert qc["snapshots"]["mismatches"][0]["bids_missing_or_wrong"] == [(10001, 300)]


def test_qc_detects_a_corrupted_normalized_file(raw, tmp_path):
    golden_with_periodic_snapshot(raw)
    out = normalized(raw, tmp_path)
    path = part(out) / "book.parquet"
    table = pq.read_table(path)
    qty = table.column("qty").to_pylist()
    kinds = table.column("kind").to_pylist()
    i = next(i for i, (k, p) in enumerate(zip(kinds, table.column("px").to_pylist())) if k == KIND_DELTA and p == 10001)
    qty[i] = 999
    pq.write_table(table.set_column(table.schema.get_field_index("qty"), "qty", pa.array(qty, pa.int64())), path)
    assert run_qc(part(out))["verdict"] == "fail"


def test_qc_detects_a_crossed_book(raw, tmp_path):
    write_golden_stream(raw, T0)
    raw.depth(T0 + 7 * MS, 106, 106, bids=levels(("100.05", "1")))  # bid above the best ask
    out = normalized(raw, tmp_path)
    qc = run_qc(part(out))
    assert qc["book"]["crossed"] == 1 and qc["verdict"] == "fail"


# -- packer -----------------------------------------------------------------------------------------------------

def test_pack_writes_a_bse_file_in_receive_order(raw, tmp_path):
    golden_with_periodic_snapshot(raw)
    out = normalized(raw, tmp_path)
    run_qc(part(out))
    with pytest.raises(PackError, match="partial"):
        pack_dataset(out, tmp_path / "packed", [SYMBOL], [DATE])
    d = pack_dataset(out, tmp_path / "packed", [SYMBOL], [DATE], name="ds", allow_partial=True)
    header, rec = read_bse(d / f"{SYMBOL}.bse")
    book = pq.read_table(part(out) / "book.parquet")
    trades = pq.read_table(part(out) / "trade.parquet")
    assert header["record_count"] == book.num_rows + trades.num_rows == len(rec)
    assert header["symbol"] == SYMBOL and header["tick_size"] == 1_000_000 and header["lot_size"] == 100_000
    assert header["price_decimals"] == 2 and header["qty_decimals"] == 3
    assert header["first_ts_local"] == rec["ts_local"][0] and header["last_ts_local"] == rec["ts_local"][-1]
    assert bool((np.diff(rec["ts_local"]) >= 0).all())
    kinds = rec["kind"].tolist()
    # The trade (received at +5 ms) sits between the deltas received at +4 ms and +6 ms
    t = kinds.index(KIND_TRADE)
    assert rec["seq"][t - 1] == 103 and rec["seq"][t + 1] == 105 and rec["px"][t] == 10003
    m = json.loads((d / "manifest.json").read_text())
    assert m["qc_verdict"] == "partial" and m["files"][0]["sha256"] and m["files"][0]["dates"][0]["date"] == DATE
    assert m["files"][0]["source_manifest_sha256"] == header["source_manifest_sha256"]


def test_pack_refuses_unchecked_failed_changed_or_mixed_spec_data(raw, tmp_path):
    t0 = DAY2 - 600 * SEC
    raw.exchange_info(t0 - SEC, tick="0.01")
    raw.snapshot(t0, 100, levels(("100.00", "1")), levels(("100.10", "1")))
    raw.exchange_info(DAY2 - 60 * SEC, tick="0.1")
    raw.depth(DAY2 + SEC, 101, 101, bids=levels(("100.00", "2")))
    out = normalized(raw, tmp_path)
    dates = [DATE, "2026-01-02"]
    with pytest.raises(PackError, match="no QC report"):
        pack_dataset(out, tmp_path / "p", [SYMBOL], dates, allow_partial=True)
    for d in dates:
        run_qc(part(out, d))
    with pytest.raises(PackError, match="trading rules change"):
        pack_dataset(out, tmp_path / "p", [SYMBOL], dates, allow_partial=True)
    with pytest.raises(PackError, match="not normalized"):
        pack_dataset(out, tmp_path / "p", [SYMBOL], ["2026-01-03"], allow_partial=True)
    (part(out, DATE) / "trade.parquet").write_bytes(b"tampered")
    with pytest.raises(PackError, match="changed since it was normalized"):
        pack_dataset(out, tmp_path / "p", [SYMBOL], [DATE], allow_partial=True)


# -- catalog ----------------------------------------------------------------------------------------------------

def test_catalog_lists_and_verifies_datasets(raw, tmp_path):
    golden_with_periodic_snapshot(raw)
    out = normalized(raw, tmp_path)
    run_qc(part(out))
    d = pack_dataset(out, tmp_path / "packed", [SYMBOL], [DATE], name="ds", allow_partial=True)
    assert [m["dataset"] for m in catalog.datasets(tmp_path / "packed")] == ["ds"]
    rows = catalog.normalized(out)
    assert rows[0]["qc_verdict"] == "partial" and rows[0]["snapshots"] == "2/2"
    assert catalog.verify(d) == []
    bse = d / f"{SYMBOL}.bse"
    data = bytearray(bse.read_bytes())
    data[-1] ^= 0xFF
    bse.write_bytes(bytes(data))
    assert any("SHA-256" in p for p in catalog.verify(d))
    assert catalog.main(["--packed", str(tmp_path / "packed"), "verify", "ds"]) == 1
