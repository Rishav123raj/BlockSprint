# Files: src/sim/bse.hpp, mmap_file, replayer, l2_book.hpp, reconstruct, sha256.hpp

How the simulator reads recorded market data. Python packs a day of normalized Binance data into one `.bse` file per symbol ([research.md](research.md), `research/data/pack.py`); everything here reads those files back. The full design is in [docs/data_pipeline.md](../docs/data_pipeline.md) and [docs/simulator_architecture.md](../docs/simulator_architecture.md).

---

## The `.bse` format (`bse.hpp`)

A 512-byte header, then fixed 40-byte records, all little-endian.

| Header field | Meaning |
| :--- | :--- |
| magic, version, record size, record count | `"BSEVENT\0"`, 1, 40; the file size must match the count |
| first / last `ts_local` | The time range, ns since the epoch |
| source manifest SHA-256 | Ties the file to the dataset it was packed from |
| symbol, tick size, lot size, decimals, min notional | The symbol's trading rules (1e-8 units). The simulator sets them as the engine's `SymbolSpec` ([symbol_spec.md](symbol_spec.md)) |

| Record field | Meaning |
| :--- | :--- |
| `ts_local` | When the recorder received the event: the simulator's clock |
| `ts_exch` | Binance's event time |
| `px`, `qty` | Price in **ticks**, quantity in **lots** (multiply by tick and lot size for 1e-8 units) |
| `seq` | Low 32 bits of the update ID or trade ID |
| `kind` | `SNAP_BEGIN`, `SNAP_LEVEL`, `SNAP_END`, `DELTA`, `TRADE`, `GAP` |
| `side` | `BID`/`ASK`; for a trade, the aggressor's side (`BID` = buy) |
| `flags` | `LAST_IN_BATCH`, `FROM_SNAPSHOT`, `DUPLICATE_SUSPECT`, `TS_CLAMPED`, `SYNTHETIC_SNAPSHOT`, `OFF_GRID` |

`parse_header` rejects a wrong magic, version or record size, non-zero reserved bytes and a size that doesn't match the count. `write_file` builds fixtures for tests.

## `MmapFile`

Maps a whole file read-only (`CreateFileMapping`/`MapViewOfFile` on Windows, `mmap` on POSIX). The replayer reads records straight from the mapping, so a day of data costs no heap memory and the OS pages it in as the replay advances. Move-only.

## `Replayer`

Hands out a file's records as **batches**, the units in which the market changes:

| `BatchKind` | Records |
| :--- | :--- |
| `DEPTH` | Consecutive `DELTA`s up to the one flagged `LAST_IN_BATCH` (one Binance depth event) |
| `SNAPSHOT` | `SNAP_BEGIN`, `SNAP_LEVEL`…, `SNAP_END` |
| `TRADE` | One `TRADE` record |
| `GAP` | One `GAP` record: updates were lost and a new segment starts |

A `Batch` is a `[begin, end)` range over the mapping plus its file index. Two fields are filled in by the simulator: `shift` (permanent impact in ticks when the batch was applied, P6.6) and `symbol` (the file's index in a multi-symbol run, P6.7). `next()` throws on a truncated depth event or snapshot, or an unknown record kind, naming the record index.

## `L2Book` (`l2_book.hpp`)

A pure L2 book: quantity per price, in ticks and lots, holding exactly what the data says (no strategy orders). It is the reference the synthesizer is checked against, and the strategies' `ObservedBook` is built on it.

Layout (P3.18): each side keeps a **dense window** of quantities indexed by tick around its best price, plus an ordered map for levels outside it. 94.5% of BTCUSDT depth updates fall within ±65,536 ticks of the market, while Binance's 5,000 levels per side can span 10 M ticks, so the window takes the traffic at array speed and the map holds the far levels cheaply. When the best price leaves the middle half of the window, the window is re-centred. Integer-only, so the same operations give the same book everywhere.

## Reconstruction (`reconstruct.hpp`)

`Reconstructor` replays a file's book batches into an `L2Book` with the rules of the Python oracle (`research/data/refbook.py`):
- `SNAP_BEGIN` clears the book (invalid until `SNAP_END`), `SNAP_LEVEL` sets a level, `SNAP_END` makes it valid;
- `DELTA` sets a level's absolute quantity (0 removes it);
- `GAP` clears the book and invalidates it until the next snapshot;
- `TRADE` doesn't change the book.

Uses:

| Function | What for |
| :--- | :--- |
| `reconstruct` + checkpoint CSVs | Validation gate V0: `bs_sim reconstruct` must equal the Python oracle (`research/sim/check_reconstruct.py`) |
| `slice` | `bs_sim slice`: the book at `from` as one synthetic snapshot, then every batch until `to`. A small file that replays on its own (test fixtures, the sample dataset) |
| `bbo_timeline` | The data's best bid and ask over time, timed as the simulator times batches (analytics' mark, P5.4) |
| `trade_list` | The data's trades (market VWAP, P5.5) |
| `level_quantities` | The data's quantity at given prices and times (queue position at entry, P5.7) |

The last three are exposed to Python as `data_bbo`, `data_trades` and `data_level_qty` ([bindings.md](bindings.md)).

## `sha256.hpp`

A small streaming SHA-256 (FIPS 180-4) for run metadata: the config, manifest and output hashes. Not for secrets.
