# Simulator benchmark (P3.18)

`bench_sim FILE.bse [--repeat N] [--stages scan,reconstruct,synth,null,quoter]` (`bench/bench_sim.cpp`) times each layer of the simulator on one `.bse` file:

| Stage | What runs |
| :--- | :--- |
| `scan` | `Replayer` only: header validation, batch iteration over the memory-mapped file |
| `reconstruct` | + the L2 book (reconstruction mode, gate V0's path): the plan's "replay-only" figure |
| `synth` | + the synthesizer into the matching engine (L2 → L3), no strategy |
| `null` | the full `Simulator` loop (event queue, observed book, gateway) with a strategy that never trades |
| `quoter` | the full loop with `PassiveQuoter` (0.001, 500 ms, post-only, 10 ms constant latency) |

## Results (2026-11-10)

**Data:** BTCUSDT 2026-11-06, cut from the packed Nov 5–7 dataset with `bs_sim slice --from 2026-11-06 --to 2026-11-07`. That's 26.8 M records over 24 h, 74.65% of the day covered (the PC slept 07:40–13:45).

**Machine and build:** Intel Core i5-6300U (2.4 GHz, 2 cores, laptop), Windows 10, MSVC Release. Each stage ran in its own process; the table shows the best of 2 runs.

| Stage | Before | After | Speed-up |
| :--- | ---: | ---: | ---: |
| scan | 1.10 s (24.3 M rec/s) | 0.74 s (36.4 M rec/s) | 1.5× |
| reconstruct | 6.22 s (**4.32 M rec/s**) | 1.64 s (**16.3 M rec/s**) | 3.8× |
| synth | 39.3 s | 21.6 s | 1.8× |
| sim null | 47.4 s | 24.5 s | 1.9× |
| sim quoter | **50.1 s** | **25.1 s** (25.1–26.0 over 5 runs) | 2.0× |

The quoter run: 7.28 M events, 20,463 orders, 86,305 fills.

## Plan targets

| Target | Result |
| :--- | :--- |
| Replay-only ≥ 5 M events/s | **met:** 16.3 M records/s |
| One-day BTCUSDT `PassiveQuoter` run < 30 s | **met on the best recorded day:** 25.1 s for 26.8 M records (74.65% coverage). A fully covered day would have about 36 M records, about **34 s** at the same rate, so not yet met for a complete day |

## What changed

1. **L2 book** (`src/sim/l2_book.hpp`). Each side is a dense window of 2^17 ticks indexed by price, kept centred on the best price, with an ordered map for the levels outside it (it was a `std::map` per side). On BTCUSDT 94.5% of depth updates fall within ±65,536 ticks of the market, while the 5,000 levels per side span up to 10 M ticks (stray orders far away). Reconstruction, the synthesizer's data book and every strategy's observed book use it. A differential test against a `std::map` reference with 8- and 64-tick windows (2.8 M checks) covers the far map and window moves.
2. **Synthesizer: one queue read per changed level.** `reconcile` read each level twice, and a shrinking level three times (`remove` rescanned). A change at one price never alters the synthetic quantity at another: synthetic orders don't trade with each other, and a crossing fill takes strategy orders only. So for a depth event (each price once, checked) the quantity left after the decreases is what the increases start from. Snapshots keep the old path, because they list a level twice.

Both changes leave every result identical: the pinned determinism hashes (P3.17), golden scenarios and invariant tests pass unchanged.

## Where the time goes now

Measured with `rdtsc` counters on a 2-hour slice, GCC build, synth stage:

| Part | Share |
| :--- | ---: |
| engine calls for synthetic orders: submit 1.68 G cycles, modify 1.16 G, cancel 1.16 G | 43% |
| reading a level's queue (`find_level`, a `std::pmr::map` lookup in the engine) | 14% |
| tape trades (one IOC each) | 8% |
| the rest of `reconcile` and the data book | 35% |

Two other leads were checked:
- In the simulator loop, the event queue, dispatch and publishing are about 10%. The synthesizer is 85%.
- Link-time optimisation (MSVC `/GL`) gave no gain.

The next step is in the engine: integer order handles instead of string IDs, an engine-owned order pool instead of `make_shared` per order, and a faster price-level index. That is the P1.18 note's "P3.18 path". It changes the matching core shared with the live server, so it is a separate decision.

## Reproduce

```
cmake --build build --config Release --target bench_sim bs_sim
build\Release\bs_sim.exe slice data/packed/binance_spot_btc_eth_1105_1107/BTCUSDT.bse --from 2026-11-06 --to 2026-11-07 --out btc_1106.bse
build\Release\bench_sim.exe btc_1106.bse --repeat 2
```
