# Analytics: what a run earned, and why

`research/analytics/` turns a run's tables into numbers (Phase 5). Every function takes a run: a `Results` (in memory, `research/sim/results.py`) or a `StoredRun` (from the store, `docs/experiments.md`).

## The market the run saw (`market.py`)

```python
from research.analytics.market import MarketData
md = MarketData.for_run(run)        # replays the run's .bse file up to the window's end
md.mid(ts_ns)                       # the data's mid at each time
md.bbo_at(ts_ns)                    # bid, bid_qty, ask, ask_qty
```

- **Source:** `blocksprint_sim.data_bbo(path, from_ns, to_ns)` (C++, `bbo_timeline` in `src/sim/reconstruct.*`). It rebuilds the book from the file with the simulator's rules and returns one row per change of the best bid or ask. A whole day of BTCUSDT (Nov 10: 638,520 rows) takes about 5 s, and the last four timelines are cached.
- **Exact:** the mid is the simulator's mark, the same integer in 1e-8 units. A test checks this against every sample of `positions.csv`.
- **Two lookups at a time t,** because events at the same nanosecond happen in order:
  - `side="right"` gives the book after every data batch up to and including t. A fill sees this, since the batch that caused it has been applied.
  - `side="left"` gives the book before batches at t. The simulator's samples and its final mark see this.
- **Invalid book:** before the first snapshot the mid is NaN. In a gap, the last valid mid is carried forward.

## PnL and its parts (`pnl.py`, P5.4)

```python
from research.analytics.pnl import decompose, pnl_buckets, fills
decompose(run)               # one row per strategy
pnl_buckets(run, "1m")       # per time bucket: the run's returns
fills(run)                   # each fill with its mid and edge
```

In quote units, with the data's mid m(t) as the mark (as `positions.csv`'s `pnl`):

| Part | Definition |
| :--- | :--- |
| **spread capture** | per fill, q × (m(t_fill) − price), q signed (+ buy). Positive when buying below the mid or selling above it. Split into `spread_maker` and `spread_taker` |
| **inventory** | over a bucket [a, b): pos(a) × (m(b) − m(a)), plus q × (m(b) − m(t_fill)) for each fill in it. The position times the mid's moves |
| **fees** | paid (negative: rebates) |

**pnl = spread capture + inventory − fees**, exactly, in every bucket:
- The parts add up to the change in cash + position × m − fees.
- The cumulative pnl at the bucket edges equals `positions.csv`'s `pnl` at those times. A test checks this at every 1 s sample.
- `decompose` also reports `residual`, the engine's final pnl minus the sum. It is 0 up to float rounding, unless a data gap made the simulator mark at its own book.

**Other columns:**
- `decompose`: fills (maker / taker), volumes, the final position and mark, and `edge_bps` (spread capture per unit of notional, in basis points).
- `pnl_buckets`: buckets are `[edge, edge + every)` from the end of the warm-up (`start="window"` includes it), and the last one may be shorter. Each has fills, volume, the three parts, `pnl` (the bucket's return), `cum_pnl`, and the position and mid at its end. `every` takes `"500ms"`, `"1s"`, `"5m"`, `"1h"`, an int in ns or a `timedelta`.

**Example:** `configs/sim/example_btc.json` (passive quoter, BTCUSDT Nov 5 02:00–08:00, Tokyo latency, 0.1% fees):

| pnl | spread capture | inventory | fees | fills | edge |
| ---: | ---: | ---: | ---: | ---: | ---: |
| −346.18 | −3.37 | −34.05 | 308.76 | 17,093 maker | −0.11 bps |

The quoter sits at the best bid and ask, so a fill comes when the market trades through its price. The mid has usually moved against it by then, so spread capture is slightly negative. The inventory it is left with keeps losing (adverse selection). Fees at 0.1% are 90% of the loss. Per hour the picture is the same, from −46 to −81.

## Transaction cost analysis (`tca.py`, P5.5)

```python
from research.analytics import tca
tca.summary(run)            # one row per strategy
tca.orders(run)             # one row per placement
tca.fill_costs(run, "5s")   # per fill: effective / realized spread, price impact
tca.slippage(run)           # per strategy and side: vs the market's VWAP and the mid's TWAP
tca.rejects(run)            # refusals by reason
```

**Costs are positive when paid,** in basis points of the mid unless a column is in quote units. s is +1 for a buy and −1 for a sell.

| Measure | Definition |
| :--- | :--- |
| **placement** | one version of an order: a NEW, or a REPLACE (same order ID; the old version ends `cancelled` / `replaced`) |
| **implementation shortfall** | s × (average fill price − m(sent)) on the filled part. m(sent) is the mid when the strategy sent the order: the arrival price |
| **opportunity cost** | s × (m(end) − m(sent)) × the unfilled quantity, at the placement's end (cancel, replace, or the run's end) |
| **effective spread** | 2s(price − m(t)) per fill |
| **realized spread** | 2s(price − m(t + h)): the cost once the market has moved on (h: `horizon`, default 5 s) |
| **price impact** | 2s(m(t + h) − m(t)) = effective − realized. Negative for a maker means adverse selection |
| **slippage** | s × (average fill price − benchmark) over the side's execution interval, from its first placement sent to its last fill. Benchmarks: the data's trade VWAP (`blocksprint_sim.data_trades`) and the mid's TWAP |
| **fill ratio** | filled ÷ placed quantity (rejected placements excluded) |
| **time to fill** | from a placement's arrival at the exchange to its first fill, and to its last fill when it filled completely |
| **cancel / fill** | cancellations (by the strategy, and by replaces) per fill report |
| **order / trade** | requests sent per fill report |
| **rejects** | `order` for a NEW / REPLACE the exchange rejected, else the refused cancel / amend / replace, each with its reason |

**On real data:**
- **Passive quoter** (`configs/sim/example_btc.json`, 6 h, 17,093 fills; TCA takes 1.4 s):
  - 7,924 placements; fill ratio 48%; time to first fill 2.0 s median, 12.1 s p90;
  - effective spread +0.22 bps, realized +1.81 bps at 5 s, impact −1.59 bps (adverse selection);
  - 4,077 cancels by replace, 0.24 cancels per fill;
  - 13 orders rejected, 3 requests refused.
- **TWAP buy** of 1 BTC over 50 min (Nov 10 14:00–15:00, 60 market slices): 1.4% of the market's volume; 0.71 bps better than the VWAP, 0.10 bps better than the mid's TWAP; impact +1.23 bps at 5 s.

## Markouts (`markouts.py`, P5.6)

```python
from research.analytics.markouts import fill_markouts, markout_curve
fill_markouts(run)                       # each fill: mo_<h>_bps and mo_<h> (quote) per horizon
markout_curve(run)                       # averages per horizon, by liquidity and side
markout_curve(run, by=())                # all fills together
markout_curve(run, np.geomspace(1e6, 6e10, 30).astype(int), by=("liquidity",))   # a dense curve
```

**Definition:** the markout of a fill at horizon h is s × (m(t + h) − price). That is the fill's profit if the position were closed at the mid h later.
- **Sign:** positive is good for the strategy, unlike `tca.py`, where positive is a cost.
- **Units:** in bps it is divided by the mid at the fill, m(t). Fees are not included.
- **Horizons:** 0, 100 ms, 1 s, 5 s, 30 s and 60 s by default.
- **Consistency with the other modules:**
  - at h = 0 the markout is the fill's spread capture (P5.4);
  - the change after that, s × (m(t + h) − m(t)), is half of TCA's price impact.
  
  A test checks both.
- **Past the end of the data:** a horizon that falls beyond the run's data has no mid. That markout is null and left out of the averages (`fills` counts the fills used).

**The curve's columns:** per strategy, group and horizon: `fills`, `qty`, `mean_bps` (weighted by quantity), `median_bps`, `se_bps` (the standard error of the unweighted mean) and `pnl` (the sum in quote).

**On real data:**

| Horizon | 0 | 100 ms | 1 s | 5 s | 30 s | 60 s |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| Passive quoter, 17,093 maker fills (`configs/sim/example_btc.json`), bps | −0.11 | −0.33 | −0.58 | −0.90 | −1.01 | −1.13 |
| TWAP buy, 85 taker fills (Nov 10 14:00–15:00), bps | −0.03 | −0.05 | +0.14 | +0.59 | +0.81 | +0.61 |

- **Quoter:** the curve falls steadily, which is adverse selection. The standard errors are 0.001–0.04 bps, and the buy and sell sides look alike. Its 60 s markout loses 34.83 USDT, about the inventory loss in the P5.4 split.
- **TWAP:** a taker pays a little at once. After that the market rose that afternoon, so its later markouts are positive, but with only 85 fills the standard errors (0.3–0.9 bps) are as large as the means.

The markouts for the 6-hour run take 0.9 s.

## Queue analytics (`queue.py`, P5.7)

```python
from research.analytics import queue
queue.placements(run)              # one row per passive placement
queue.fill_curve(run)              # fill probability by queue ahead at entry (10 quantile bins; or bins=[edges])
queue.fill_curve(run, by="ticks")  # ... by distance from the best price
queue.summary(run)                 # one row per strategy
```

- **Passive placements:** limit and post-only placements (from `tca.orders`) that were accepted and never took liquidity.
- **Queue ahead at entry** (`ahead_qty`, and `ahead_ratio` in multiples of the order's own size): the displayed quantity already at the placement's price when it reached the exchange. It comes from the data via `blocksprint_sim.data_level_qty`, a C++ replay that answers any number of (time, side, price) queries at once. So it is the same for every queue model; the models differ in how fast that quantity drains.
- **`ticks_from_best`:** 0 joined the best price, > 0 sat behind it, < 0 improved it (a new level, nothing ahead).
- **Time in queue:** from arrival to the first fill, or to the end (cancel, replace, the run's end).
- **`traded_at_price` / `traded_through`:** the data's trades by the other side's aggressors while the order rested, at its price and beyond it.
- **`fifo_reachable`:** those trades exceed the queue ahead, so trades alone would have reached the order under first-in-first-out.
  - Orders that weren't reachable still fill. The queue ahead also shrinks when the displayed level is cancelled down past it (how much depends on the queue model) and when the market moves through the price.
  - `summary` gives the fill rate of both groups.

**On real data,** passive quoter `configs/sim/example_btc.json` (cancel_from_back, Tokyo latency): 7,911 placements; the query takes 1.6 s.
- **Where it joined:** 99.9% at the best price.
- **Queue ahead:** median 5.7 BTC, i.e. 5,700 times its own 0.001.
- **Fill rates:** 48% overall. Only 6.6% were reachable by trades alone; those all filled, and 45% of the rest filled too.

| Queue ahead at entry (BTC) | < 0.05 | 0.05–0.31 | 0.31–0.87 | 0.87–2.0 | 2.0–5.7 | 5.7–8.2 | 8.2–10.3 | 10.3–11.8 | 11.8–13.2 | > 13.2 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fill rate | 95% | 89% | 82% | 75% | 49% | 24% | 20% | 18% | 15% | 17% |
| Median time to fill (s) | 0.26 | 0.81 | 1.78 | 2.37 | 4.73 | 6.27 | 5.84 | 7.75 | 8.06 | 8.30 |

**Paper vs simulator (P7.5).** `research/analytics/parity.py` compares a paper session with its replays: fill ratio, time to fill, spread and markouts against the recorded data's mid, and the PnL split; see [parity_protocol.md](parity_protocol.md). Calibration (P7.6) fits the simulator to those reports: [calibration.md](calibration.md).

**Multi-symbol runs (P6.7).** Every function here measures one symbol against its own data. Narrow a multi-symbol run first: `tca.fill_costs(run.for_symbol("ETHUSDT"), "5s")`; an un-narrowed one raises `ValueError`.

## Risk (`risk.py`, P5.8)

```python
from research.analytics import risk
risk.summary(run, "1m", position_limit=0.01)   # one row per strategy: all of the below
risk.ratios(run, "1m")       # Sharpe, Sortino, hit rate from the PnL per bucket
risk.drawdown(run)           # max drawdown, its peak, trough and recovery; drawdown_series(run) for the curve
risk.inventory(run)          # time-weighted position distribution
risk.limits(run)             # time at the position / open-order limits, limit rejects
risk.turnover(run)           # volume, per hour, inventory turnover, holding time
```

Everything is measured from the end of the warm-up.

| Measure | Definition |
| :--- | :--- |
| **Sharpe / Sortino** | from the PnL of each bucket (`pnl_buckets`, default 1 minute; a shorter last bucket is left out). Mean ÷ standard deviation, and mean ÷ downside deviation (root mean square of the losses, the gains counted as 0). Annualized by √(buckets in 365 days). They are ratios of PnL, so no capital figure is needed. They mean little with few buckets (`buckets`), and they are extreme for a strategy that loses steadily |
| **drawdown** | on the engine's mark-to-mid PnL at every sample: the largest fall from a running high; when the high and the low were; the first sample back at the high (null if never); the duration |
| **inventory** | the exact position path from the fills, weighted by time: mean, mean absolute, standard deviation, min, max, 5 / 50 / 95% quantiles, the time flat / long / short, and the mean absolute notional at the mid |
| **limits** | the time with \|position\| ≥ the limit (`position_limit`, else the strategy's `max_position` parameter if the config sets it; a C++ strategy's default isn't in the config); the share of samples with `limits.open_orders` open orders; the orders refused for the open-order limit, every request (orders, cancels, amends, replaces) refused for the order-count or request-weight limits, every request the strategy's own RiskGuard refused (`risk_*` reasons), and every request refused while its gap policy halted trading (`gap_halted`) |
| **turnover** | base and quote volume, quote volume per hour, inventory turnover (base volume ÷ mean \|position\|) and holding time (mean \|position\| ÷ (base volume ÷ 2 per second)) |

**On real data,** passive quoter `configs/sim/example_btc.json` with its default `max_position` of 0.01 (the summary takes 1.5 s):
- **Ratios:** the 1-minute PnL is −0.96 ± 0.52 USDT, and no minute gained. Sharpe −1,344 and Sortino −638 annualized: a strategy that loses every minute.
- **Drawdown:** 346.18. The PnL never rose above 0, so the drawdown is the whole loss, over all 5 h 59 m.
- **Inventory:** mean +0.0035 BTC, mean absolute 0.0063. Long 73% of the time, short 26%, flat 1%. At the ±0.01 limit 7.9% of the time; mean absolute notional 505 USDT.
- **Turnover:** 51,604 USDT per hour; inventory turned over 611 times, a holding time of about 71 s.

## Tearsheets and comparison reports (`report.py`, P5.9)

`python -m research.run EXPERIMENT.yaml` writes them after the runs (`--no-report` skips them):

```text
results/experiments/<name>/report.html            the comparison: heatmaps over the swept parameters, every run
results/experiments/<name>/runs/<run_id>.html     a tearsheet per run
```

```python
from research.analytics import report
report.write_tearsheet(run, "tearsheet.html")      # a Results or a StoredRun
report.write_reports(result, experiment)           # what research.run does
```

```text
python -m research.analytics.report RUN_ID [--results results] [--out FILE]   # one stored run
```

**What's on a tearsheet:**
- **Settings and headline figures:** PnL, its three parts, fills, fill ratio, max drawdown, the 5 s markout.
- **Charts:**
  - the mark-to-mid PnL, and its drawdown;
  - the PnL split, cumulative;
  - the position;
  - the markout curve by liquidity and side;
  - the fill rate by queue ahead at entry.
- **Tables:** TCA, risk and rejects.
- **PnL buckets:** the longest of 1 s … 1 h that gives at least 30 (`auto_every`), unless `every` is given.

**What's on the comparison:**
- **Heatmaps** of PnL, fills, the 5 s markout and max drawdown. They are drawn over each pair of swept parameters (up to three pairs), averaged over seeds and the other parameters. With one swept parameter they are a single row.
  - Signed metrics run red, through gray at 0, to blue; magnitudes use the blue ramp.
  - A failed point is an empty cell.
- **A table of every run,** linked to its tearsheet, with failures listed.

**How the pages are built:**
- **Self-contained:** the charts are inline SVG, and a few lines of inline script give hover and keyboard read-outs. Nothing is loaded from the network. Every chart has its numbers in a table below it. Light and dark themes come from the system setting.
- **Byte-identical on a rerun:** the pages carry no times and don't say whether a run came from the store, so running an experiment again rewrites the same bytes. A test checks this.

**Phase 5 acceptance:** `python -m research.run research/experiments/configs/mm_btc.yaml` runs the quoter on 1 hour of Nov 10 BTCUSDT: 3 refresh intervals × 3 position limits.
- **First run:** 9 runs in 29.5 s with 4 workers, then the reports in about 10 s.
- **Second run:** all 9 runs come from the store, and the 10 pages are byte-identical.
- **What the heatmaps show:**
  - Faster refreshing and a larger position limit bring more fills: 1,350 to 2,286.
  - They also bring bigger losses: −33 to −51.
  - The 5 s markout is about −1.15 bps everywhere.

Since P5.10, `mm_btc.yaml` sweeps the Avellaneda–Stoikov market maker's γ × k instead; its results are in [strategies.md](strategies.md).

## Tests (P5.13)

The analytics are tested in `tests/python/analytics/` (46 tests), in two ways:
- **Known answers:** each module has small synthetic cases (a few fills and a hand-written book) whose results are worked out by hand in the test's comments. For example, a buy of 2 at 99.99 with the mid at 100, which moves to 101, has a 100 ms markout of +2.02. Cases cover:
  - buys and sells, maker and taker fills, and a short position that turns long;
  - bucket edges, horizons past the end of the data, and an invalid book;
  - queue placements that join, improve or sit behind the best, and quantile bins that collapse to one;
  - rejects, drawdown and recovery, and time at a position limit.
- **Consistency on the committed one-minute slice:** the PnL split adds up to the engine's PnL, the mid matches the simulator's mark, the markouts agree with TCA, and a stored run gives the same numbers as a fresh one.

A run that sends no orders gives empty tables, with their usual columns, and zero or NaN summaries, never an error. Its tearsheet still renders.
