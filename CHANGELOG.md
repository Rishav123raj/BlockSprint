# Changelog

All notable changes to BlockSprint. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

Defect IDs (D1–D22) refer to the 1.0 review in [backtesting_simulator_integration_guide.md](backtesting_simulator_integration_guide.md). Task IDs (P1.6, …) refer to [blocksprint_2.0_implementation_plan.md](blocksprint_2.0_implementation_plan.md).

## [Unreleased]

### Added

- A nightly workflow (`.github/workflows/nightly.yml`): 20 new fuzz seeds each night (10⁶ operations, each seed logged for replay), and the README's quickstart on fresh Windows and Linux runners, which checks the sample run's output hash.
- Queue positions (`output.queue_positions: true`, off by default): `queue_positions.csv` and `Results.queue_positions` follow each resting strategy order's place in its queue in the engine (quantity ahead, its own, behind), with the cause of every change. Other outputs and pinned hashes are unchanged.
- A gitleaks scan of the whole history on every push (`.gitleaks.toml`: the default rules, with only the 1.0 commit f5d4b49 exempt).

### Security

- The Binance API key committed in 1.0 was deleted on Binance on 2026-11-17; the values left in the history no longer work.

## [2.0.0] - 2026-11-15

2.0 turns BlockSprint from a live matching engine with a Binance mirror into a **market-microstructure-aware backtesting and execution simulator**, with a live paper mode on the same core. The matching engine became a deterministic, integer-priced library; recorded Binance data replays through it with queue and latency models; strategies are written once in C++ or Python and run unchanged in the simulator and in paper mode.

### Breaking changes

#### REST and WebSocket API

Existing 1.x clients keep working, except for the points below.

| Endpoint / message | 1.x | 2.0 |
| :--- | :--- | :--- |
| Order with a duplicate `order_id` | Silently cancelled and replaced the resting order (D9) | **400 `duplicate_order_id`**. Replace explicitly with `PUT /api/orders` or `"action": "replace"` |
| LIMIT / IOC / FOK without a price | The price became 0, so a sell swept every bid (D6) | **400 `invalid_price`** |
| Prices and quantities | Strings parsed with `std::stod`, silently rounded | Strings **or** numbers, parsed exactly; anything finer than 1e-8 is a 400. Responses still use strings |
| Symbols with trading rules | No validation (D12) | Orders off the tick or lot grid, out of the quantity range or below the minimum notional are rejected (`price_tick`, `quantity_lot`, `quantity_range`, `min_notional`). Quantity ≤ 0 is always rejected |
| `GET /api/bbo`, `GET /api/depth` for an unknown symbol | Created an empty book (D18) | **404** |
| Rejected order | — | HTTP 400 (WebSocket `"status": "error"`) with the reason as `message`. Order responses gain a `reports` array next to `trades` |
| `/ws/market-data` depth events | One per order, trade or BBO change | Coalesced per symbol: at most one every 100 ms, always the latest book. BBO events still go out on every change |
| Fees | None | The live server charges Binance's regular spot tier, 0.10% maker / 0.10% taker, by default (`POST /api/fees` changes it). Fees appear in execution reports only |
| Mirrored liquidity | Identified by the `BINANCE_` order ID prefix, which anyone could spoof (D8) | Owner `MARKET` with `"stp": "skip"`, accepted only from localhost. `MARKET_TAPE` is reserved for the market feed |
| Self-trade prevention | Prefix-based skip only | Per-order `owner` and `stp` (`none`, `skip`, `cancel_taker`, `cancel_maker` (default, as on Binance), `cancel_both`) |
| `/ws/executions` | — | Excludes the reports of `MARKET` and `MARKET_TAPE` orders |

#### C++ API

- **`MatchingEngineManager` (a singleton) is now `MatchingEngine`, an ordinary class** (D3): each instance has its own books, trade IDs from 1 (D2), sequence numbers, clock and fee schedules. `EngineOptions::thread_safe` turns the mutex off for single-threaded simulation.
- **Prices and quantities are `int64_t` in 1e-8 units** (`Price`, `Quantity`, `types.hpp`) instead of `double` (D5). `parse_fixed` / `format_fixed` convert at the boundaries.
- **Time comes from an injected `IClock`** (`WallClock` or `SimClock`); `getCurrentTimestampNs()` is gone from the core (D1).
- Mutating calls take an optional `std::vector<ExecReport>*` and there is an exec-report callback (D4). `replace_order` and `amend_order(id, new_total, new_price?)` are new. `amend_order` takes the new **total** quantity, as on Binance (D11); `modify_order` is kept for 1.x callers.
- Source layout: `src/` split into `src/core` (the `engine_core` library) and `src/api` (the `api` library).
- Logger: off until `Logger::open(LoggerOptions{...})`; each run writes its own file (D19).

#### Build and tooling

- The test executable is `unit_tests` (doctest), replacing `run_tests`. Simulator tests are in `sim_tests`.
- `external/asio` and `external/websocketpp` are proper git submodules: `git clone --recursive` (D22, which meant 1.0 couldn't build from a fresh clone).
- Python 3.13 or later, managed with `uv`. `uv sync` builds the `blocksprint_sim` module, so it needs a C++17 compiler ([docs/dev_setup.md](docs/dev_setup.md)).
- `.env` is no longer tracked, and the connector uses only public endpoints, so it needs no API keys.

#### Connector

- `scripts/binance_connector.py` v1 (top-10 partial book re-submitted as orders, D10) is **removed**, with its `--v1` flag (deprecated at `v2.0.0-p7`, removed after the soak test in P7.8). `scripts/binance_connector.py` now runs only connector v2, `research/live/feed_bridge.py`, which streams every diff-depth update into `/ws/market-feed`.

### Added

#### Core engine (Phase 1)
- **Execution reports** for every order's whole lifecycle (`ack`, `fill`, `amended`, `cancelled`, `rejected`), each with a reason. The contract: one terminal report per accepted order, two fills per trade. Also the `/ws/executions` stream.
- Order types `limit_maker` (alias `post_only`) and FOK that never rests; amend through `PATCH /api/orders` and `"action": "amend"`.
- Symbol trading rules (`POST`/`GET /api/symbols`) mirroring Binance's filters, loaded by the connector.
- **Maker/taker fees** per owner plus a default (`POST`/`GET /api/fees`), exact to 1e-8 with 128-bit arithmetic, rebates allowed.
- `check_invariants()`, a fuzz test against a reference book (10⁶ operations in its own CI job), a determinism test, and `bench_core`. Hot-path work: one clock read per call, an intrusive FIFO per level and a node pool per book (half of D15), and no JSON on the hot path (D13).
- Server options `--port`, `--log-*`, `--feed-*`.

#### Market data (Phase 2)
- **Recorder** for Binance spot depth, trades and snapshots, with clock-quality metadata, a hourly-rotated zstd raw archive, and a keep-awake guard on Windows.
- **Normalizer** to Parquet (Binance's sync rules, gaps as segments, duplicate drop), a **Python reference book**, a **QC suite** with HTML reports, the **`.bse` replay format** and its packer, a catalog, and a downloader for `data.binance.vision`.
- **Sample dataset:** one hour of BTCUSDT and ETHUSDT, downloadable with `python -m research.data.sample get`.

#### Simulator (Phase 3, `bs_sim`)
- A discrete-event loop over memory-mapped `.bse` files, ordered by (time, class, sequence), with a time window, warm-up and tie-break option.
- **L2 → L3 synthesis:** data levels become synthetic FIFO orders in the real matching engine, and trades become tape IOCs, so a strategy's fills come from the engine's own matching. Queue models: `cancel_from_back`, `cancel_from_front`, `pro_rata`, `prob_power:n`, `prob_log`.
- **Latency:** feed, entry and ack channels with constant, normal, lognormal and empirical models, never overtaking; presets.
- **Gateway** with Binance's request limits; two accounts per strategy (the exchange's and what the strategy knows); the strategy's own delayed `ObservedBook`.
- The C++ `Strategy` interface and reference strategies `null`, `passive_quoter` and `twap_taker`.
- JSON run configs (defaults < preset < file < `--set`; unknown keys are errors), a results directory with five CSVs, `run_meta.json` and an output hash.
- **Deterministic across compilers:** its own RNG and `log`/`exp`, with pinned output hashes checked on MSVC and GCC.
- `bs_sim info | reconstruct | synth | latency | config | run | slice`, and validation gates V0–V4.

#### Python (Phases 4–5)
- **`blocksprint_sim` module:** runs, configs, **Python strategies** with the GIL released during the replay (`every_event`, `on_change` and `timer_only` callback modes), and zero-copy numpy results with polars conversion.
- `research.strategies.Strategy` base class, and strategies Avellaneda-Stoikov, order-book imbalance taker, TWAP/POV executor, and a Python port of the passive quoter that matches the C++ output hashes.
- **Experiments:** YAML sweeps (grid or random, several seeds), a content-addressed results store, a parallel runner and `python -m research.run`.
- **Analytics:** PnL decomposition, TCA, markouts, queue analytics, risk metrics, HTML tearsheets and comparison reports. Three notebooks.

#### Realism (Phase 6)
- Impact half-life for liquidity a strategy took, and permanent square-root impact (off by default).
- Latency that rises with the market's message rate.
- `RiskGuard`: per-strategy max position, order notional, open orders, and a drawdown kill switch (`risk_*` rejections).
- Gap policies `halt` (default), `flatten` and `continue` (`gap_halted` rejections).
- Multi-symbol runs.

#### Live and paper mode (Phase 7)
- **`/ws/market-feed`:** the live server builds its book with the simulator's own Synthesizer, so paper trading and backtests share one L2 → L3 path. `GET /api/market-feed` reports the feed's fidelity, and `gap` events go to market-data subscribers.
- **Connector v2** (`research/live/feed_bridge.py`), the **live strategy runner** (a research strategy, unchanged, in paper mode), and the **paper session protocol** with a parity report.
- **Calibration** of the queue model and latency against paper sessions, with held-out seeds; a calibrated BTCUSDT profile; latency measured on the Binance Spot Testnet.
- A soak test for connector v2, and a CI guard that no code signs Binance requests (the testnet tool is the single exception).

#### Documentation
- Design documents in `docs/` (simulator architecture, data pipeline, live platform, parity protocol, calibration, experiments, analytics, strategies, developer setup), `explanations/` for every component, and a README with a quickstart on the sample data.
- Baselines: `docs/baseline/v1.0.0.md`, `bench_core.md`, `bench_sim.md`.

### Fixed

| Defect | Fix |
| :--- | :--- |
| D1 Wall-clock time inside matching | Injected `IClock` |
| D2 Process-global trade ID counter | Per-engine IDs |
| D3 Singleton engine | `MatchingEngine` instances |
| D4 No execution reports | The report stream |
| D5 `double` price keys | Fixed-point `int64` |
| D6 Missing price became 0 | `invalid_price` rejection |
| D7 Self-trade prevention could leave the book crossed | A remainder that would cross is cancelled (`would_cross`). The connector was also a live source of crossing, and was fixed |
| D8 Spoofable `BINANCE_` prefix | Owners and STP policies |
| D9 Duplicate ID cancelled the resting order | `duplicate_order_id` rejection |
| D10 Connector reset queue priority every 100 ms | Connector v2 streams changes into the Synthesizer |
| D11 Ambiguous modify quantity | `amend_order` with the new total |
| D12 No order validation | Symbol trading rules |
| D13 JSON on the hot path | Plain structs; JSON only for subscribers (15–85× faster) |
| D14 Matching logic duplicated six times | One sweep routine |
| D15 `shared_ptr` + `std::list` per order | Intrusive FIFO and node pool (half fixed; orders are still `shared_ptr`) |
| D16 `recursive_mutex` around everything | Optional locking |
| D17 Dead lazy-delete branch | Removed |
| D18 Getters created books | 404 |
| D19 Logger: hard-coded path, flush per line | Configurable, buffered, one file per run |
| D20 Documentation drift | Fixed |
| D21 FOK could partially fill and rest | A FOK fills completely or not at all |
| D22 The repo couldn't build from a fresh clone | Submodules registered |

Also fixed in the 1.0 connector: REST snapshots fetched with blocking calls inside the async loop left books half-built.

### Security

- A Binance API key and secret were committed to this repository's history in 1.0. **That key must be considered compromised and revoked.** The history wasn't rewritten. Since 2.0 no code needs mainnet keys, `.env` isn't tracked, and CI rejects any code that signs Binance requests, except the testnet tool.

### Known limitations

- Paper mode is simulated fills against a mirrored book, not real orders; strategy orders don't change what other participants do.
- Calibration meets the markout target on 2 of 3 paper sessions; `docs/calibration.md` has the details. The Tokyo VPS latency preset is an estimate.
- The connector v2 soak test (2026-11-14, 26 h, [docs/baseline/soak_2026-11-14.md](docs/baseline/soak_2026-11-14.md)) was accepted with one criterion failed: resyncs took up to 116 s during a loss of the test PC's network (under 10 s once Binance was reachable). It did not observe Binance's own 24 h connection close (the outage replaced the connection first) and did not show where the server's memory for the live books levels off (about 2.3 MB/h over 22 h, released when the books reset).

## [1.0.0] - 2026-07-04

The first release: a live L3 matching engine for crypto spot pairs.

- Price-time priority order book with LIMIT, MARKET, IOC and FOK orders, cancel and modify.
- REST (`POST`/`DELETE /api/orders`, `GET /api/bbo`, `GET /api/depth`, `POST /api/reset`) and WebSocket (`/ws/market-data`, `/ws/trades`, `/ws/orders`) API on WebSocket++.
- A Binance connector mirroring the top 10 levels of the live book as orders.
- A test harness and a performance script.

[Unreleased]: https://github.com/Rishav123raj/BlockSprint/compare/v2.0.0...main
[2.0.0]: https://github.com/Rishav123raj/BlockSprint/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/Rishav123raj/BlockSprint/releases/tag/v1.0.0
