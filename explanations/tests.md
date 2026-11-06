# Tests and benchmarks (tests/core, bench)

How the engine is tested. Everything in `tests/core` builds into one doctest binary, `unit_tests`, that links only `engine_core` (no networking). CI builds and runs it on Windows (MSVC) and Linux (GCC) for every push, plus a separate 10^6-operation fuzz job ([.github/workflows/ci.yml](../.github/workflows/ci.yml)).

```
cmake -B build && cmake --build build --config Release
./build/Release/unit_tests            # all tests (Linux: ./build/unit_tests)
./build/Release/unit_tests -ts=fuzz   # one suite
```

---

## Test files

| File | What it pins |
| :--- | :--- |
| `matching_test.cpp` | The six original 1.0 tests: price-time priority, trade-through protection, IOC, FOK, cancel, modify |
| `characterization_test.cpp` | 1.0 behavior pinned before the 2.0 refactors (P1.3): sweeps, FIFO, modify priority, depth, callbacks, and two **golden hashes** over a seeded 5,000-operation random workload |
| `types_test.cpp` | Fixed-point parsing and formatting; D5 |
| `invariant_test.cpp` | `check_invariants()` after every operation of random workloads, including mirrored liquidity and every STP policy |
| `engine_test.cpp` | Independent engines, per-engine trade IDs, the injected clock, locking on and off, concurrent use, resubmitting a resting order object |
| `stp_test.cpp` | Every STP policy; D7, D9, D21; replace |
| `exec_report_test.cpp` | The report contract ([events.md](events.md)) on hand-made cases, and a lifecycle checker on every report of 20 random workloads, fees included |
| `validation_test.cpp` | Symbol rules; D6, D18 |
| `fees_test.cpp` | Fee rounding, extremes, per-owner schedules, rebates |
| `post_only_amend_test.cpp` | Post-only orders and Binance-style amend; D11 |
| `logger_test.cpp` | Levels, buffering, per-run files, concurrent writers; D19 |
| `fuzz_test.cpp` | Engine vs a naive reference book (below) |
| `determinism_test.cpp` | Bit-identical output across replays, compilers and platforms (below) |

Shared helpers: `test_helpers.hpp` (`fx()` for fixed-point values, `make_order`, `EngineFixture`) and `workload.hpp` (portable RNG, FNV-1a hash, the seeded random workload behind the golden hashes).

## Golden hashes

The characterization test hashes every trade of a seeded random workload. Without self-trade prevention the hash equals 1.0's exactly, so 2.0 matches the same way. The variant with mirrored liquidity was re-pinned once, at P1.9, when D7 and D21 changed self-trade behavior on purpose. A changed hash means a changed matching outcome somewhere.

## Fuzz test (P1.16)

Seeded random operations go to the engine and to a deliberately naive reference book: one flat vector of resting orders, re-sorted by price and arrival on every use, sharing only `fee_for` with the engine. The operations cover all order types, replace, cancel, modify, amend, owners with every STP policy, mirrored liquidity, per-owner fees, invalid and duplicate orders, unknown IDs and symbols, and two books.

After every operation the test compares the return values, trades, full exec-report streams and every level of both books, and runs `check_invariants()`. It also requires every report type and reason to occur at least once. It runs 10^5 operations here and 10^6 in CI. `BLOCKSPRINT_FUZZ_OPS` sets the operations per seed and `BLOCKSPRINT_FUZZ_SEED` replays one seed. A failure names the seed, the operation and the first field that differs. When it was written, 8 of 8 injected engine bugs were caught.

## Determinism test (P1.17)

A seeded script of operations, with their times, is replayed into fresh `SimClock` engines. It uses three symbols, one with trading rules. One FNV-1a hash covers the canonical JSON of every exec report (sequence numbers, timestamps and rejection texts included), every trade, BBO and depth update, every return value and the final books. Two replays, and an engine with locking off, must agree. The value is pinned (`kGoldenReportStreamHash`), and MSVC and GCC produce the same one, so CI checks it on both platforms. Change it only when the engine's output changes on purpose, in the same commit, saying why.

## Benchmark (bench/bench_core.cpp)

Engine throughput without networking: `passive` (rest, then cancel), `matching` (a trade-heavy limit/market/IOC/cancel mix) and `mirror` (replacing 20 mirrored levels, like the Binance connector). Each runs as `sim` (no locking, `SimClock`, no callbacks) and `live` (locking, wall clock, every callback). It prints engine-only ops/s and ops/s including building each order. Results and their history: [docs/baseline/bench_core.md](../docs/baseline/bench_core.md).
