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
