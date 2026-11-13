# Paper-vs-simulator parity protocol (P7.4)

How to check the simulator against reality: run a strategy in **paper mode** against the live market while the
**recorder** captures the same window, then **replay** that window in the simulator with the same strategy and
parameters. The two runs see the same market; what differs is what the simulator models (queue positions,
latency, the book it shows) and what paper mode does for real. The parity report (P7.5) compares them, and
calibration (P7.6) tunes the simulator until they agree.

Paper mode trades against the server's mirror of Binance, not Binance itself. It has the simulator's assumption
that the strategy's orders don't move the market, so it checks **latency and queue modelling against live timing,
not market impact**. Only real small-size trading checks impact, and that is outside 2.0.

## One command per step

```powershell
# 1. Record and paper-trade one window (about minutes + 2 minutes in all)
uv run python -m research.live.paper_session run `
    --strategy research.strategies.passive_quoter:PassiveQuoter `
    --params '{"size": "0.001", "refresh_ms": 500, "max_position": "0.005"}' `
    --symbol BTCUSDT --minutes 15 --session data/paper/2026-11-14_quoter_btc_1

# 2. Replay the same window in the simulator, once per seed
uv run python -m research.live.paper_session replay --session data/paper/2026-11-14_quoter_btc_1 --seeds 1 2 3
```

Both need Binance's public endpoints; the run also needs the server built (`cmake --build build --config Release`).
Sessions go under `data/paper/` (not in git). Run at least three sessions, at different times of day, per strategy
and setting before drawing conclusions (the Phase 7 acceptance asks for three).

## What `run` does

1. **Recorder first.** `research.data.recorder` writes the symbol's raw `@depth@100ms`, `@trade` and REST snapshots
   into `<session>/raw`, from `--lead-s` (default 20 s) before the session to `--tail-s` (default 60 s) after it.
   The lead gives the recording a snapshot and a live book before the strategy starts; the tail gives markouts
   data past the last fill.
2. **Server and feed.** A fresh server on `--port` (default 8095) with the session's `--queue-model` (default
   `prob_power:2`), then the v2 feed bridge for the symbol. The session waits until the feed reports `synced`.
3. **Paper trading.** The live runner runs the strategy class, unchanged, for `--minutes`, and journals into
   `<session>/paper/`: `session.jsonl` (every request, report and refusal, gaps, a per-second sample of the top of
   book and the account), `session_meta.json` (counters, accounts, the measured delays) and `feed_stats.json`
   (the server's fidelity counters at the end). Orders still open at the end are cancelled.
4. **Stop.** The bridge and the server stop; the recorder records its tail and exits on its own.

`protocol.json` holds every input: the strategy class and parameters, callback mode, symbol (Binance's and the
engine's name), queue model, owner, lead and tail.

## What `replay` does

1. **Dataset.** The session's raw archive is normalized, QC'd and packed into `<session>/packed/session` with
   `--allow-partial` (a session covers minutes of a day, so its date's verdict is `partial`). A QC `fail` stops it.
2. **Same window, same strategy.** For each seed, the simulator runs the strategy class from `protocol.json` with
   its parameters and callback mode over exactly `[trading start, end)` of the paper session, with:

   | Setting | Paper | Replay |
   | :--- | :--- | :--- |
   | queue model | the server's `--feed-queue-model` | the same |
   | fees | 0.1% / 0.1% (engine default) | the same (config default) |
   | gap handling | the server broadcasts `gap`; nothing is halted | `gap_policy: continue` |
   | latency | real | from what the paper session measured (below), or `--latency` |
   | book shown | the server's top 10, every ≤ 100 ms, without own orders | the full observed book at feed latency |
   | order rules | the server's (no request rate limits) | the gateway's Binance limits (far above these strategies) |

3. **Output.** `<session>/sim/seed_<n>/` holds the five tables as Parquet and `run_meta.json`;
   `<session>/sim/replay.json` the latency used and each seed's fills, volume and PnL.

### Latency from the session

The runner measures two delays during the paper session (`session_meta.json`, `measured_latency`):

- **`md_delay_ms`:** from the server's book change to the strategy receiving the depth update. The server sends
  depth at most every 100 ms, so this is roughly uniform on 0–100 ms (trial: mean 47 ms, p10 8 ms, p90 89 ms).
- **`order_rtt_ms`:** from sending a new order to its first report (trial: median 3.5 ms, p90 10.7 ms).

The replay's default latency (`paper_session.default_latency`) turns them into a feed delay that is normal with
that mean and the spread implied by p10–p90 (the simulator has no uniform model), and entry and ack delays of half
the median round trip each. This is the starting point; P7.6 fits it.

## Known differences, by construction

- **Depth is batched.** Binance sends depth every 100 ms but trades as they happen; the server's feed reproduces
  about half of the trades exactly at their price (the rest meet levels the next depth update changes). The
  recorded data the simulator replays has the same property, so both sides see it; `feed_stats.json` reports it
  per session.
- **The paper strategy sees 10 levels** (the server's market-data stream), the simulated one the whole book.
  Strategies that read deep levels can differ for that reason alone.
- **Clocks.** The recorder, bridge, server and runner run on one machine and stamp with the same wall clock, so the
  paper window maps exactly onto the recording.
- **Requests the server can't refuse.** The server has no request rate limits; the simulator applies Binance's.
  A strategy near those limits differs for that reason.

## Trial run (2026-11-14, 3 minutes)

`PassiveQuoter` (0.001 a side, 500 ms, at most 0.005) on BTCUSDT, `prob_power:2`:

| | Fills | Volume (BTC) |
| :--- | ---: | ---: |
| Paper | 146 | 0.047 |
| Replay (seeds 1, 2, 3) | 131 each | 0.045 each |

The three seeds have different output hashes (the queue model and feed delay are random) but the same fill totals
on this short window. Measured: depth delay mean 47 ms, order round trip median 3.5 ms.

## First session (2026-11-14, 15 minutes)

`data/paper/2026-11-14_quoter_btc_1`: the same quoter and settings, BTCUSDT, about 19:00 UTC on 2026-11-13.

| | Fills | Volume (BTC) |
| :--- | ---: | ---: |
| Paper | 447 | 0.168 |
| Replay seed 1 | 395 | 0.176 |
| Replay seed 2 | 396 | 0.172 |
| Replay seed 3 | 402 | 0.176 |

- **Paper:** 366 requests, 987 reports, no refusals, no gaps; every fill a maker fill.
- **Measured:** depth delay mean 46 ms (p10 8, p90 88); order round trip median 3.1 ms (p90 5.4).
- **The server's feed:** 19,977 trades, 55% reproduced exactly, 0.108 BTC of them filled the quoter's orders.
- **Data:** QC `partial` (1.25% of the day), both snapshots matched, no crossed books.
- **First reading:** the simulator trades 2-5% more volume in fewer, larger fills (395-402 against 447). The volume
  is already well inside the ±15% target; the fill count, markouts and the rest are P7.5's report. One session is
  not enough to conclude anything: the acceptance needs at least three.
