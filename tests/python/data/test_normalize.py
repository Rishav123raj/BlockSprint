"""Normalizer (P2.7, P2.8): golden output and the sync / gap / duplicate / day-boundary rules."""

import json

import pyarrow.parquet as pq
import pytest
from conftest import DAY1, DAY2, MS, SEC, SYMBOL, levels, write_golden_stream

from research.data.normalize import normalize_symbol
from research.data.schema import (
    FLAG_DUPLICATE_SUSPECT, FLAG_FROM_SNAPSHOT, FLAG_LAST_IN_BATCH, FLAG_OFF_GRID, FLAG_SYNTHETIC_SNAPSHOT,
    FLAG_TS_CLAMPED, KIND_DELTA, KIND_GAP, KIND_SNAP_BEGIN, KIND_SNAP_END, KIND_SNAP_LEVEL, KIND_TRADE,
    partition_dir,
)

T0 = DAY1 + 3600 * SEC  # 01:00 UTC
COLS = ("ts_local", "ts_exch", "seq", "seq_first", "seg", "kind", "side", "px", "qty", "flags")


def rows(raw_root, out, name, date="2026-01-01"):
    table = pq.read_table(partition_dir(out, SYMBOL, date) / name)
    return [tuple(r[c] for c in COLS) for r in table.to_pylist()]


def meta(out, date="2026-01-01"):
    return json.loads((partition_dir(out, SYMBOL, date) / "meta.json").read_text())


def run(raw, tmp_path, dates=None):
    raw.close()
    out = tmp_path / "norm"
    normalize_symbol(raw.root, out, SYMBOL, dates)
    return out


def test_golden_output(raw, tmp_path):
    write_golden_stream(raw, T0)
    out = run(raw, tmp_path)
    snap_ts = T0 + 3 * MS
    snap = FLAG_FROM_SNAPSHOT
    # Tick 0.01 and lot 0.001: 100.00 -> 10000 ticks, 1.5 -> 1500 lots
    assert rows(raw.root, out, "book.parquet") == [
        (snap_ts, 0, 100, 100, 0, KIND_SNAP_BEGIN, 0, 0, 0, snap),
        (snap_ts, 0, 100, 100, 0, KIND_SNAP_LEVEL, 0, 10000, 1500, snap),
        (snap_ts, 0, 100, 100, 0, KIND_SNAP_LEVEL, 0, 9999, 3000, snap),
        (snap_ts, 0, 100, 100, 0, KIND_SNAP_LEVEL, 1, 10002, 1000, snap),
        (snap_ts, 0, 100, 100, 0, KIND_SNAP_LEVEL, 1, 10003, 2000, snap),
        (snap_ts, 0, 100, 100, 0, KIND_SNAP_END, 0, 0, 0, snap | FLAG_LAST_IN_BATCH),
        # Received before the snapshot, applied after it: moved to the snapshot's time and flagged.
        # The event 95..99 is older than the snapshot and dropped.
        (snap_ts, (T0 + 2 * MS) // MS * MS, 102, 99, 0, KIND_DELTA, 0, 10000, 2000, FLAG_TS_CLAMPED | FLAG_LAST_IN_BATCH),
        (T0 + 4 * MS, T0 + 4 * MS, 103, 103, 0, KIND_DELTA, 1, 10002, 0, FLAG_LAST_IN_BATCH),
        (T0 + 6 * MS, T0 + 6 * MS, 105, 104, 0, KIND_DELTA, 0, 10001, 250, 0),
        (T0 + 6 * MS, T0 + 6 * MS, 105, 104, 0, KIND_DELTA, 1, 10003, 1500, FLAG_LAST_IN_BATCH),
    ]
    assert rows(raw.root, out, "trade.parquet") == [
        (T0 + 5 * MS, T0 + 5 * MS, 500, 500, 0, KIND_TRADE, 0, 10003, 500, 0),
    ]
    # The REST snapshot is also kept for QC
    assert len(rows(raw.root, out, "snapshot.parquet")) == 6
    m = meta(out)
    assert m["spec"]["tick_size"] == 1_000_000 and m["spec"]["lot_size"] == 100_000
    assert m["stats"]["depth_events"] == 3
    assert m["stats"]["duplicate_suspect_events"] == 0  # the first event after a sync may straddle L
    assert m["stats"]["ts_clamped_rows"] == 1
    assert m["day_complete"] is False
    assert set(m["files"]) == {"book.parquet", "trade.parquet", "snapshot.parquet"}


def test_gap_starts_a_new_segment_that_resyncs_from_the_next_snapshot(raw, tmp_path):
    write_golden_stream(raw, T0)
    raw.depth(T0 + 10 * MS, 110, 111, bids=levels(("100.00", "9")))   # 106..109 missing
    raw.depth(T0 + 11 * MS, 112, 112, bids=levels(("100.00", "8")))   # buffered
    raw.trade(T0 + 12 * MS, 501, "100.03", "0.1", buyer_is_maker=True)
    raw.snapshot(T0 + 13 * MS, 111, levels(("100.00", "9")), levels(("100.05", "1")))
    raw.depth(T0 + 14 * MS, 113, 113, asks=levels(("100.05", "2")))
    out = run(raw, tmp_path)
    book = rows(raw.root, out, "book.parquet")
    gap = [r for r in book if r[5] == KIND_GAP]
    assert gap == [(T0 + 10 * MS, T0 + 10 * MS, 105, 110, 1, KIND_GAP, 0, 0, 0, FLAG_LAST_IN_BATCH)]
    after = book[book.index(gap[0]) + 1:]
    assert [r[5] for r in after] == [KIND_SNAP_BEGIN, KIND_SNAP_LEVEL, KIND_SNAP_LEVEL, KIND_SNAP_END,
                                    KIND_DELTA, KIND_DELTA]
    assert all(r[4] == 1 for r in after)
    # 110..111 is covered by the snapshot (dropped); 112 continues it; 113 follows
    assert [(r[2], r[3]) for r in after if r[5] == KIND_DELTA] == [(112, 112), (113, 113)]
    # Trades are kept through the gap, tagged with the segment current when received
    assert [(r[2], r[4], r[6]) for r in rows(raw.root, out, "trade.parquet")] == [(500, 0, 0), (501, 1, 1)]
    m = meta(out)
    assert m["stats"]["gaps"] == 1
    assert m["segments"][1]["missed_updates"] == 4


def test_a_snapshot_older_than_the_buffer_is_skipped(raw, tmp_path):
    write_golden_stream(raw, T0)
    raw.depth(T0 + 10 * MS, 120, 121, bids=levels(("100.00", "9")))   # gap
    raw.snapshot(T0 + 11 * MS, 110, levels(("100.00", "1")), levels(("100.05", "1")))  # 111..119 missing: unusable
    raw.depth(T0 + 12 * MS, 122, 122, bids=levels(("100.00", "7")))
    raw.snapshot(T0 + 13 * MS, 121, levels(("100.00", "9")), levels(("100.05", "1")))
    out = run(raw, tmp_path)
    m = meta(out)
    assert m["stats"]["sync_attempts_failed"] == 1
    book = rows(raw.root, out, "book.parquet")
    seg1 = [r for r in book if r[4] == 1]
    assert seg1[1][2] == 121  # synced from the second snapshot
    assert [r[2] for r in seg1 if r[5] == KIND_DELTA] == [122]


def test_reconnect_overlap_duplicates_are_dropped(raw, tmp_path):
    write_golden_stream(raw, T0)
    # A second connection opens before the first closes and repeats what was already applied
    raw.depth(T0 + 7 * MS, 104, 105, bids=levels(("100.01", "0.25")), conn=2)
    raw.trade(T0 + 8 * MS, 500, "100.03", "0.5", buyer_is_maker=False, conn=2)
    raw.depth(T0 + 9 * MS, 105, 107, bids=levels(("100.01", "0.3")), conn=2)  # overlaps 105
    raw.depth(T0 + 10 * MS, 106, 107, bids=levels(("100.01", "0.3")), conn=1)
    raw.trade(T0 + 11 * MS, 503, "100.03", "0.1", buyer_is_maker=True, conn=2)  # 501, 502 missing
    out = run(raw, tmp_path)
    m = meta(out)["stats"]
    assert m["depth_duplicates_dropped"] == 2
    assert m["duplicate_suspect_events"] == 1
    assert m["trade_duplicates_dropped"] == 1
    assert m["trade_id_gaps"] == 1 and m["trades_missing"] == 2
    deltas = [r for r in rows(raw.root, out, "book.parquet") if r[5] == KIND_DELTA]
    assert deltas[-1][2:4] == (107, 105) and deltas[-1][9] == FLAG_DUPLICATE_SUSPECT | FLAG_LAST_IN_BATCH
    assert meta(out)["stats"]["gaps"] == 0


def test_clock_stepping_back_is_clamped(raw, tmp_path):
    write_golden_stream(raw, T0)
    raw.depth(T0 + 2 * MS, 106, 106, bids=levels(("100.01", "1")))  # received "earlier" than the last row
    out = run(raw, tmp_path)
    last = rows(raw.root, out, "book.parquet")[-1]
    assert last[0] == T0 + 6 * MS and last[9] & FLAG_TS_CLAMPED


def test_off_grid_values_are_flagged(raw, tmp_path):
    write_golden_stream(raw, T0)
    raw.depth(T0 + 7 * MS, 106, 106, bids=levels(("100.005", "1")))  # half a tick
    out = run(raw, tmp_path)
    last = rows(raw.root, out, "book.parquet")[-1]
    assert last[9] & FLAG_OFF_GRID
    assert meta(out)["stats"]["off_grid_rows"] == 1


def test_each_day_starts_with_a_synthetic_snapshot_and_its_own_tick_size(raw, tmp_path):
    t0 = DAY2 - 600 * SEC  # 23:50 on day 1
    raw.exchange_info(t0 - SEC, tick="0.01")
    raw.depth(t0 + 1 * MS, 99, 101, bids=levels(("100.10", "1")))
    raw.snapshot(t0 + 2 * MS, 100, levels(("100.10", "2"), ("100.00", "1")), levels(("100.20", "1")))
    raw.exchange_info(DAY2 - 60 * SEC, tick="0.1")  # tick size changes from day 2
    raw.depth(DAY2 + 5 * SEC, 102, 102, asks=levels(("100.30", "4")))
    out = run(raw, tmp_path)

    day1, day2 = meta(out, "2026-01-01"), meta(out, "2026-01-02")
    assert day1["spec"]["tick_size"] == 1_000_000 and day2["spec"]["tick_size"] == 10_000_000
    book2 = rows(raw.root, out, "book.parquet", "2026-01-02")
    synth = FLAG_FROM_SNAPSHOT | FLAG_SYNTHETIC_SNAPSHOT
    assert book2[:5] == [
        (DAY2, 0, 101, 101, 0, KIND_SNAP_BEGIN, 0, 0, 0, synth),
        (DAY2, 0, 101, 101, 0, KIND_SNAP_LEVEL, 0, 1001, 1000, synth),  # 100.1 in 0.1 ticks; delta 99..101 set qty 1
        (DAY2, 0, 101, 101, 0, KIND_SNAP_LEVEL, 0, 1000, 1000, synth),
        (DAY2, 0, 101, 101, 0, KIND_SNAP_LEVEL, 1, 1002, 1000, synth),
        (DAY2, 0, 101, 101, 0, KIND_SNAP_END, 0, 0, 0, synth | FLAG_LAST_IN_BATCH),
    ]
    assert book2[5][5:] == (KIND_DELTA, 1, 1003, 4000, FLAG_LAST_IN_BATCH)
    assert day1["day_complete"] is True and day2["day_complete"] is False


def test_only_requested_dates_are_written_but_state_carries_over(raw, tmp_path):
    t0 = DAY2 - 600 * SEC
    raw.exchange_info(t0 - SEC)
    raw.snapshot(t0, 100, levels(("100.00", "1")), levels(("100.01", "1")))
    raw.depth(DAY2 + SEC, 101, 101, bids=levels(("100.00", "2")))
    out = run(raw, tmp_path, dates=["2026-01-02"])
    assert not partition_dir(out, SYMBOL, "2026-01-01").exists()
    book2 = rows(raw.root, out, "book.parquet", "2026-01-02")
    assert book2[0][5] == KIND_SNAP_BEGIN and book2[0][9] & FLAG_SYNTHETIC_SNAPSHOT
    assert book2[-1][7:9] == (10000, 2000)


def test_missing_exchange_info_is_an_error(raw, tmp_path):
    raw.depth(T0, 1, 1, bids=levels(("1", "1")))
    raw.close()
    with pytest.raises(ValueError, match="no exchangeInfo"):
        normalize_symbol(raw.root, tmp_path / "norm", SYMBOL)
