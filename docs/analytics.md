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
