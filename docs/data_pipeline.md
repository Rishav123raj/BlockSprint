# Market-Data Pipeline: Command Guide

How to run and check every step from the raw Binance recording to decoded replay records: record, inspect, normalize, QC, pack, catalog, decode, plus the public-data downloader and the tests. Phase 2 of [blocksprint_2.0_implementation_plan.md](../blocksprint_2.0_implementation_plan.md) (P2.0–P2.14).

```
Binance WebSocket + REST
        │  research.data.recorder        (always on; docs/recorder_ops.md)
        ▼
data/raw/binance/spot/<SYMBOL>/<date>/<HH>-<ns>.jsonl.zst      raw envelopes, hourly files
        │  research.data.inspect_raw     (optional: raw continuity report)
        │  research.data.normalize       (P2.7, P2.8)
        ▼
data/normalized/venue=binance/market=spot/symbol=<SYMBOL>/date=<date>/
        book.parquet  trade.parquet  snapshot.parquet  meta.json
        │  research.data.qc              (P2.10, uses research.data.refbook, P2.9)
        ▼
        qc.json  qc.html                 verdict: pass / partial / fail
        │  research.data.pack            (P2.12)
        ▼
data/packed/<dataset>/<SYMBOL>.bse  manifest.json             replay files for the C++ simulator (Phase 3)
        │  research.data.catalog         (P2.13: list, show, verify)

data.binance.vision ── research.data.downloader (P2.11) ──► trade_public / agg_trade_public / klines_1m.parquet
```

All commands run from the repository root, in PowerShell or bash. `uv run` uses the project's virtual environment; with the venv activated (`.venv\Scripts\activate`), `python -m ...` works the same way.

---

## 0. One-time setup

```powershell
uv sync                     # installs pyarrow, numpy, orjson, zstandard, websockets, requests, pytest
uv run pytest -q            # 40 tests; must all pass
```

---

## 1. Record (raw input)

The recorder runs as the scheduled task **BlockSprint Recorder** on this PC (details, power settings and the Linux VPS option: [recorder_ops.md](recorder_ops.md)).

```powershell
Get-ScheduledTask "BlockSprint Recorder" | Select-Object State     # Running?
Get-Content data\raw\_status.json                                    # updated_ns, frame counts per symbol
Get-Content logs\recorder_supervisor.log -Tail 5                     # restarts
Start-ScheduledTask "BlockSprint Recorder"                           # start
Stop-ScheduledTask  "BlockSprint Recorder"                           # stop (the open hour stays a readable .part file)
```

A manual run, for a quick test (Ctrl+C stops it cleanly):

```powershell
uv run python -m research.data.recorder --symbols BTCUSDT ETHUSDT --out data/raw --duration 300
```

**What's recorded:** one combined WebSocket per symbol group with `<sym>@depth@100ms` and `<sym>@trade`, a REST depth snapshot (5,000 levels) at every connection start and every 10 minutes, and `exchangeInfo` plus NTP clock samples under `_meta`. Every message is stored as received, with the local receive time `t` in nanoseconds:

| `k` | Meaning |
| :--- | :--- |
| `ws` | WebSocket frame, `s` = stream, `d` = the exact text received |
| `snapshot` | REST depth snapshot, `t0` = request time, `d` = response text |
| `event` | recorder lifecycle: connecting, open, closed, recorder_stopped |
| `exchange_info` | `_meta` only: trading rules (tick, lot, min notional) |
| `clock` | `_meta` only: NTP offset sample |

### Look at raw data

```powershell
uv run python -c "from research.data.rawio import archive_files, read_raw; f = archive_files('data/raw', 'BTCUSDT')[-1]; print(f); [print(str(e)[:200]) for _, e in zip(range(5), read_raw(f))]"
```

### Raw continuity report (optional)

```powershell
uv run python -m research.data.inspect_raw --root data/raw --symbols BTCUSDT ETHUSDT
```

Per connection: depth sequence gaps (must be 0), trade-ID gaps (must be 0), whether each snapshot can sync the book, handovers between connections and restart gaps (expected while the recorder wasn't running). It reads every file, so it takes a few minutes on several days of data.

---

## 2. Normalize (P2.7, P2.8)

```powershell
uv run python -m research.data.normalize --symbols BTCUSDT ETHUSDT                          # every recorded date
uv run python -m research.data.normalize --symbols BTCUSDT --dates 2026-11-05               # chosen dates
uv run python -m research.data.normalize --symbols BTCUSDT --from 2026-11-04 --to 2026-11-06
```

| Option | Default | Meaning |
| :--- | :--- | :--- |
| `--raw` | `data/raw` | raw archive |
| `--out` | `data/normalized` | output root |
| `--lookback-hours` | 2 | raw data read before the first date, so the book is synced at midnight |

Output per symbol and UTC date: `book.parquet` (snapshots, depth changes and gap markers), `trade.parquet`, `snapshot.parquet` (every REST snapshot, for QC) and `meta.json` (spec in force, row counts, segments, gaps, duplicates, clamped timestamps, source files, SHA-256 of each output). Re-running a date overwrites it. A date still being recorded is written from the open `.part` file and marked `"day_complete": false`.

Speed on the development laptop: a full BTCUSDT day (~40 M book rows) takes a few minutes.

### What the normalizer does

- **Book sync (Binance's rules):** depth events are buffered until a REST snapshot arrives. Events with `u <= lastUpdateId` are dropped, and the first one kept must satisfy `U <= lastUpdateId + 1`. After that, each event must continue the previous one (`U <= prev_u + 1 <= u`).
- **Duplicates:** events with `u <= prev_u` are dropped. These come from two connections overlapping during the 24 h rollover. Duplicate trade IDs are dropped too.
- **Gaps:** an event that skips updates gets a `GAP` row and starts a new **segment**. The book then waits for the next snapshot, at most 10 minutes later.
- **Units:** prices are in **ticks** and quantities in **lots**, using the spec in force at the start of each date.
- **Self-contained days:** each date's book starts with a snapshot of the running book at midnight (`SYNTHETIC_SNAPSHOT`), so one day can be replayed on its own.
- **`ts_local` never decreases.** A row received earlier than the previous one takes the previous row's time and is flagged `TS_CLAMPED`. That covers the system clock stepping back, and depth events buffered until their sync snapshot arrived.

### Read normalized data

```powershell
uv run python -c "import pyarrow.parquet as pq; t = pq.read_table('data/normalized/venue=binance/market=spot/symbol=BTCUSDT/date=2026-11-05/book.parquet'); print(t.num_rows); [print(r) for r in t.slice(0, 8).to_pylist()]"
Get-Content data\normalized\venue=binance\market=spot\symbol=BTCUSDT\date=2026-11-05\meta.json
```

### Normalized schema (`research/data/schema.py`)

| Column | Type | Meaning |
| :--- | :--- | :--- |
| `ts_local` | int64 ns | recorder receive time (the simulation drives on this) |
| `ts_exch` | int64 ns | exchange time: depth `E`, trade `T`, 0 for snapshot rows |
| `seq` | int64 | depth: last update ID `u`; snapshot: `lastUpdateId`; trade: trade ID; gap: last `u` before it |
| `seq_first` | int64 | depth: first update ID `U`; otherwise = `seq` |
| `seg` | int32 | segment; a new one starts at every `GAP` row |
| `kind` | uint8 | 0 SNAP_BEGIN, 1 SNAP_LEVEL, 2 SNAP_END, 3 DELTA, 4 TRADE, 5 GAP |
| `side` | uint8 | book: 0 bid, 1 ask; trade: aggressor 0 buy, 1 sell |
| `px` | int64 | price in ticks |
| `qty` | int64 | DELTA: new absolute level quantity in lots (0 = removed); SNAP_LEVEL: level quantity; TRADE: traded lots |
| `flags` | uint16 | 1 LAST_IN_BATCH, 2 FROM_SNAPSHOT, 4 DUPLICATE_SUSPECT, 8 TS_CLAMPED, 16 SYNTHETIC_SNAPSHOT, 32 OFF_GRID |

To get a real price back: `px × tick_size` (e.g. 7,739,448 ticks × 0.01 = 77,394.48). Quantities work the same way with `lot_size`. Both are in `meta.json` → `spec`.

---

## 3. QC (P2.9, P2.10)

```powershell
uv run python -m research.data.qc --symbols BTCUSDT ETHUSDT                     # every normalized date
uv run python -m research.data.qc --symbols BTCUSDT --dates 2026-11-05
start data\normalized\venue=binance\market=spot\symbol=BTCUSDT\date=2026-11-05\qc.html   # open the report
```

It rebuilds the book with the Python reference book (`research/data/refbook.py`, the oracle) and writes `qc.json` and `qc.html` next to the data. The exit code is 2 if any date fails.

| Check | Pass condition |
| :--- | :--- |
| Snapshot reconstruction | every REST snapshot taken while synced equals the rebuilt book, top 20 levels per side, exactly (levels touched by the depth event that spans the snapshot's `lastUpdateId` are excluded, because their state at that ID isn't in the stream) |
| Crossed / locked book | never, after any depth event |
| Grid | no price or quantity off the date's tick / lot size |
| `ts_local` | never decreases |
| Coverage | minutes of the UTC day with a synced, updating book (≥ 99% for `pass`) |
| Latency | `ts_local − ts_exch` percentiles for depth and trades (informational; includes the PC clock offset) |
| Trades outside the BBO | informational, compared by exchange time, with a distance histogram in ticks |

**Verdicts:**
- **`pass`:** every check is clean, the day is complete, coverage is at least 99%, and at least one snapshot was verified.
- **`partial`:** every check is clean, but the day is incomplete or has gaps.
- **`fail`:** a snapshot mismatch, a crossed book, an off-grid value, or `ts_local` going backwards.

Use the reference book in your own scripts:

```python
from research.data.refbook import RefBook, iter_rows
book = RefBook()
for row in iter_rows("data/normalized/venue=binance/market=spot/symbol=BTCUSDT/date=2026-11-05/book.parquet"):
    book.apply(row)
print(book.best_bid(), book.best_ask(), book.top(5))
```

---

## 4. Pack (P2.12)

```powershell
uv run python -m research.data.pack pack --symbols BTCUSDT ETHUSDT --from 2026-11-06 --to 2026-11-08
uv run python -m research.data.pack pack --symbols BTCUSDT --from 2026-11-05 --to 2026-11-05 --allow-partial --name btc_1105
```

Before writing anything, the packer requires:
- every date to be normalized;
- every date to have a QC report with verdict `pass`, or `partial` when `--allow-partial` is given (never `fail`);
- every normalized file to still match its SHA-256 from `meta.json`;
- identical trading rules across the range. Prices are stored in ticks, so a tick-size change needs separate packs.

Output: `data/packed/<dataset>/<SYMBOL>.bse` and `manifest.json`. The default dataset name is `binance_spot_<symbols>_<from>_<to>`.

### `.bse` format v1 (little-endian)

| Header (512 bytes) | | Record (40 bytes) | |
| :--- | :--- | :--- | :--- |
| 0 magic `BSEVENT\0` | 8 version 1, 12 record size 40 | 0 `ts_local` i64 | 8 `ts_exch` i64 |
| 16 record count | 24 first / 32 last `ts_local` | 16 `px` i64 (ticks) | 24 `qty` i64 (lots) |
| 40 SHA-256 of the source manifest | 72 symbol (32 bytes) | 32 `seq` u32 (low 32 bits) | 36 `kind` u8, 37 `side` u8 |
| 104 tick, 112 lot (1e-8 units) | 120 / 121 price / qty decimals, 122 min notional | 38 `flags` u16 | |

Records are ordered by `ts_local`. At equal times book rows come before trades, and each stream keeps its own order.

### Decode a `.bse` file

```powershell
uv run python -m research.data.pack info data/packed/<dataset>/BTCUSDT.bse --records 10
```

```python
from research.data.pack import read_bse
header, rec = read_bse("data/packed/<dataset>/BTCUSDT.bse")   # rec: numpy structured array, memory-mapped
tick = header["tick_size"] / 1e8
print(header["record_count"], rec["ts_local"][0], rec["px"][:5] * tick)
trades = rec[rec["kind"] == 4]
```

---

## 5. Catalog (P2.13)

```powershell
uv run python -m research.data.catalog list                   # packed datasets with SHA-256s
uv run python -m research.data.catalog list --normalized      # every normalized date: QC verdict, coverage, snapshots
uv run python -m research.data.catalog show <dataset>         # per-date verdicts, gap minutes, segments
uv run python -m research.data.catalog verify <dataset>       # recompute every hash, check .bse headers (exit 1 on a problem)
```

---

## 6. Public data (P2.11, optional)

```powershell
uv run python -m research.data.downloader --symbols BTCUSDT --dates 2026-11-04 --datasets aggTrades klines
uv run python -m research.data.downloader --symbols BTCUSDT ETHUSDT --from 2026-11-01 --to 2026-11-04 --datasets trades
```

It downloads `data.binance.vision` daily zips into `data/downloads/binance-vision` and verifies each against its `.CHECKSUM` SHA-256; a mismatch deletes the file and fails. It then writes, into the same partition directory as the recorded data:
- `trade_public.parquet` and `agg_trade_public.parquet`, in the normalized trade schema;
- `klines_1m.parquet`;
- `public_meta.json`.

Public data has no local receive time, so `ts_local = ts_exch`. Files appear about a day after the date; an unpublished date reports "not published (yet)". Full `trades` files are large (BTCUSDT: tens of MB zipped per day). Use them to fill trades for days the recorder missed, or to cross-check recorded trades.

---

## 6b. Into the simulator: replay and validation gate V0

The C++ simulator (Phase 3, [simulator_architecture.md](simulator_architecture.md)) reads `.bse` files directly:

```powershell
cmake -B build; cmake --build build --config Release --target bs_sim sim_tests
build\Release\sim_tests.exe                                                           # simulator unit tests
build\Release\bs_sim.exe info data/packed/<dataset>/BTCUSDT.bse                        # header, batches, segments
build\Release\bs_sim.exe reconstruct data/packed/<dataset>/BTCUSDT.bse --every 5000 --top 20 --out v0_btc.csv
uv run python -m research.sim.check_reconstruct --dump v0_btc.csv --symbol BTCUSDT --dates 2026-11-05   # V0 PASS / FAIL
```

`reconstruct` replays the book from the data alone (no strategy) and dumps the top N levels per side every K book batches (and at `--at` times). The checker replays the same dates' `book.parquet` through the Python oracle and compares every checkpoint exactly. List the dates in the order they were packed. On Linux the binaries are `build/bs_sim` and `build/sim_tests`.

The synthesizer (P3.6) turns the same data into synthetic orders in the matching engine, the book strategies will trade against. To check it on a file:

```powershell
build\Release\bs_sim.exe synth data/packed/<dataset>/BTCUSDT.bse --check-every 100000              # default model: cancel_from_back
build\Release\bs_sim.exe synth data/packed/<dataset>/ETHUSDT.bse --model prob_power:2 --compare-every 0   # throughput only
```

`synth` prints how often the engine's top 20 per side equals the data after a depth event (above 99.9% on 2026-11-05; the rest are trades the depth stream hasn't reflected yet) and the number of consistency violations, which must be 0 (exit code 1 otherwise).

Delays from Binance to the recorder, for the simulator's latency models (P3.8):

```powershell
build\Release\bs_sim.exe latency data/packed/<dataset>/BTCUSDT.bse --kind depth --out btc_depth_latency.txt
```

The raw `ts_local − ts_exch` includes this PC's clock offset to Binance, which drifts by over a second a day, so the command also reports each value above the fastest of its 60 s window and writes those (µs) for `{"model": "empirical", "file": "btc_depth_latency.txt"}`.

## 7. Tests

```powershell
uv run pytest -q                                              # all Python tests (40)
uv run pytest tests/python/data/test_normalize.py -v          # normalizer: golden output, sync, gaps, duplicates, days
uv run pytest tests/python/data/test_qc_pack_catalog.py -v    # refbook, QC (incl. corruption), pack, catalog
uv run pytest tests/python/data/test_downloader.py -v         # checksums, CSV conversion (offline)
```

The fixtures (`tests/python/data/conftest.py`) write small raw archives through the recorder's own `RawWriter`. They cover:
- a normal stream with buffered events and a straddling first event;
- an injected sequence gap and its resync;
- a snapshot too old to sync;
- a reconnect overlap with duplicate depth and trades;
- a clock stepping back, and an off-grid price;
- a tick-size change across days, with each day's synthetic snapshot;
- QC catching a wrong snapshot, a corrupted Parquet file and a crossed book;
- the packer's refusals;
- the catalog detecting a tampered `.bse`.

---

## 8. End-to-end checklist

```powershell
uv run pytest -q                                                                   # 1. code OK
uv run python -m research.data.inspect_raw --root data/raw --symbols BTCUSDT ETHUSDT   # 2. raw: 0 sequence/trade gaps per connection
uv run python -m research.data.normalize --symbols BTCUSDT ETHUSDT                  # 3. normalize
uv run python -m research.data.qc --symbols BTCUSDT ETHUSDT                         # 4. QC: no FAIL; complete days PASS
uv run python -m research.data.catalog list --normalized                            # 5. overview
uv run python -m research.data.pack pack --symbols BTCUSDT ETHUSDT --from <d1> --to <d2>   # 6. pack pass days
uv run python -m research.data.catalog verify <dataset>                             # 7. hashes and headers OK
uv run python -m research.data.pack info data/packed/<dataset>/BTCUSDT.bse          # 8. decoded records
```

## Results on this archive (2026-11-06)

First full run over everything recorded so far (Nov 2–6, BTCUSDT and ETHUSDT), on the development laptop:

| Step | Result |
| :--- | :--- |
| Normalize | 10 symbol-dates in 8 min (BTCUSDT 370 s, ETHUSDT 117 s); up to 19.2 M book rows and 2.4 M trades per day |
| QC | **361 / 361 REST snapshots match the rebuilt book exactly**; never crossed; nothing off-grid; ts_local never decreases. Every date is `partial` (best: Nov 5 at 61.5% coverage) because of recorder restarts, PC sleeps and the battery sleep on Nov 6 |
| Gaps | each one is a recorder restart, a PC sleep or a Binance disconnect. Segment boundaries match `inspect_raw` exactly |
| Public cross-check | all 770,224 recorded BTCUSDT trades of Nov 4 are in Binance's public aggTrades, with the same price and aggressor side; exchange times agree to < 1 ms (the stream's ms resolution vs the file's µs) |
| Pack | Nov 5 (`--allow-partial`): BTCUSDT 21.6 M records / 866 MB, ETHUSDT 16.4 M / 658 MB, in 17 s. `catalog verify` OK. The `.bse` records split back into book and trade streams identical to the Parquet files, every field |

The plan's acceptance ("≥ 3 full clean days, QC green") needs days the recorder covers completely. Keep the PC plugged in, or move the recorder to a VPS.

## Troubleshooting

| Symptom | Cause / fix |
| :--- | :--- |
| `no exchangeInfo for X in the archive` | the recorder writes `exchangeInfo` to `_meta` daily; record at least once with this symbol |
| QC `partial` | the day has gaps or isn't complete (recorder stopped, PC slept, still recording). Check `qc.html`: the hourly coverage bars show where. Pack with `--allow-partial` only if that's acceptable |
| QC `fail` on snapshots | `qc.json` → `snapshots.mismatches` lists the levels that differ; a normalizer bug or corrupted data; don't pack it |
| pack: `changed since it was normalized` | a Parquet file was modified after normalization: normalize the date again, then QC |
| pack: `trading rules change` | Binance changed the tick/lot size inside the range: pack the periods separately |
| downloader: `not published (yet)` | data.binance.vision publishes a day's files about a day later |
| very high latency percentiles | network bursts on this PC (see recorder_ops.md); a VPS in Tokyo fixes it |
