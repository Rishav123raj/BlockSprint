# Reference strategies (Python)

Three strategies in `research/strategies/` (P5.10). They are written on the `Strategy` base class ([dev_setup.md](dev_setup.md)) and are a starting point for research, not tuned to make money. Each runs directly or from an experiment:

```python
from research.sim.results import Results
from research.strategies.avellaneda_stoikov import AvellanedaStoikov
res = Results.run(config, AvellanedaStoikov(gamma=0.005))
```

```yaml
strategy:
  python: research.strategies.avellaneda_stoikov:AvellanedaStoikov
  params: {gamma: 0.005, k: 100}
```

**Units:** quantities are decimal strings, as in the C++ strategies (`"0.001"`), and times are in milliseconds. A bad parameter raises `ValueError` when the strategy is built, and an experiment reports it when the file is loaded.

## `execution:Executor`: TWAP and POV

Works a parent order of `quantity` on one `side`. Every `interval_ms` (default 1 s) it sends the shortfall against its schedule:

- **`mode: twap`:** quantity × (elapsed ÷ `duration_ms`). The schedule starts at the first timer after the warm-up, and the order is done by `duration_ms`.
- **`mode: pov`:** `participation` × the market's traded volume since the start. The volume is the data's trades, which don't include the strategy's own fills.
- **`style: market`** (default): each shortfall goes as a MARKET order.
- **`style: passive`:**
  - One post-only order rests at the strategy's own best price, allowed to run up to `max_behind` of the quantity (default 0.1) ahead of the schedule.
  - It is re-priced only when that price moves or the order must shrink, because a replace loses the queue place.
  - If the strategy falls more than `max_behind` behind, the excess goes at market. At a TWAP's deadline everything left does.
- **Slices:** below `min_slice` (default 0.0001) they wait, except the last. Quantities round down to the lot.

## `avellaneda_stoikov:AvellanedaStoikov`: inventory-skewed market making

Avellaneda and Stoikov (2008), with a constant horizon τ (`horizon_s`):

    reservation r = m − q·γ·σ²·τ,   spread δ = γ·σ²·τ + (2/γ)·ln(1 + γ/k),   quotes r ∓ δ/2

- **The quantities:**
  - m is the mid the strategy sees.
  - q is its inventory in units of `size`, as the paper counts shares.
  - σ² is the mid's variance per second: an exponentially weighted mean of (Δmid)² ÷ Δt over the refreshes, with half-life `vol_halflife_s`, starting at `sigma0`². The estimate runs during the warm-up too.
- **What happens to the quotes:**
  - They are rounded away from the mid to the tick.
  - They are kept at least `min_spread_ticks` apart around the mid, half on each side. With the default of 1 and BTCUSDT's one-tick spread, they can join the best bid and ask.
  - A quote that would cross is moved inside the opposite best. They are post-only.
  - A side whose next fill would take |position| beyond `max_position` isn't quoted.
  - The quotes are re-priced every `refresh_ms` by replace, and cancelled during a data gap.
- **Defaults:** γ 0.0005, k 100, τ 30 s, σ₀ 2.0 USDT/√s. They are a starting point for BTCUSDT, not a calibration.

## `imbalance_taker:ImbalanceTaker`: order-book imbalance

**Signal:** I = (bid quantity − ask quantity) ÷ (bid + ask) over the best `levels` levels. It is read on each change of the best prices (`on_change`).

**Trading:**
- **When flat,** and `cooldown_ms` after its last exit: I ≥ `threshold` buys `size` at market; I ≤ −`threshold` sells.
- **When in a position:** it closes at market after `hold_ms`, or as soon as I turns against it past `exit_threshold`.

One position at a time. It takes liquidity, so it pays the spread and the taker fee on every trade.

## On real data

BTCUSDT, 2026-11-10 02:00–03:00 UTC, Tokyo latency, cancel_from_back, 0.1% fees:

| Strategy | Fills | PnL | Spread capture | Inventory | Fees | 5 s markout (bps) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| TWAP buy 0.5 BTC over 50 min, market | 3,000 taker | −62.27 | −0.01 | −24.80 | 37.46 | −0.03 |
| TWAP buy 0.5, passive (`max_behind` 0.2) | 672 maker | −84.79 | −0.46 | −46.85 | 37.48 | −1.23 |
| POV buy 0.5 at 2% | 144 taker | −119.09 | −0.05 | −81.52 | 37.51 | −0.14 |
| Avellaneda–Stoikov, defaults | 1,530 maker | −39.98 | −0.66 | −3.49 | 35.82 | −1.21 |
| Imbalance taker, defaults | 2,419 taker | −178.51 | −0.01 | +1.30 | 179.80 | +0.04 |

- **Executions:** the three buy the same 0.5 BTC. For them the inventory column is mostly the market's move while they bought, not a cost of the method. Done faster, POV bought earlier into a falling price. The passive TWAP's resting bids were filled when the price fell through them: adverse selection, as its −1.23 bps markout shows.
- **Avellaneda–Stoikov:** it keeps the inventory small; the inventory part is only −3.49.
- **Imbalance taker:** with the defaults, the imbalance predicts nothing at 5 s (+0.04 bps), so the taker fees are the whole loss.

`research/experiments/configs/mm_btc.yaml` sweeps Avellaneda–Stoikov's γ × k over the same hour. Higher γ quotes wider and leans harder against the inventory: fills fall from about 1,530 (γ 0.0005) to about 530 (γ 0.05), and the loss from about −40 to about −15. k matters little at these values.

## Sensitivity to latency and the queue model (P5.12)

`python -m research.run research/experiments/configs/sensitivity.yaml` runs the standard study: a suite of 16 runs per strategy, 48 in all, in about 3.5 minutes with 4 workers.
- **The sweep:** latency 0 / 5 / 20 / 50 ms on every leg × the queue models front / pro rata / power 2 / back.
- **The settings:** the same hour and fees as above.
  - **The market maker:** Avellaneda–Stoikov with γ 0.0005 and k 1000, so its quotes join the best bid or ask for about a fifth of its placements.
  - **The passive TWAP:** buys 0.5 BTC over 50 minutes.
  - **The imbalance taker:** default settings.
- **The output:** each strategy has a comparison report at `results/experiments/sensitivity_*/report.html`.

| | Fills (0 ms → 50 ms) | PnL (0 ms → 50 ms) | 5 s markout (bps) |
| :--- | :--- | :--- | :--- |
| **Market maker**, queue front | 1,734 → 1,446 | −49.79 → −43.81 | −1.04 → −1.00 |
| market maker, back | 1,587 → 1,340 | −40.08 → −34.19 | −1.21 → −1.19 |
| **Passive TWAP**, queue front | 895 → 845 | −83.57 → −83.51 | −0.61 → −0.62 |
| passive TWAP, back | 672 → 638 | −84.79 → −84.66 | −1.23 → −1.24 |
| **Imbalance taker** (any queue model) | 2,483 → 2,406 | −183.50 → −177.58 | +0.04 |

- **The queue model matters most for resting orders.**
  - For the market maker, the optimistic model (`front`: cancellations ahead of an order move it up) gives the most fills and the biggest loss. The extra fills are adversely selected.
  - For the passive TWAP, the front model fills a third more passively, at a better 5 s markout (−0.61 against −1.23 bps). Its PnL hardly moves, because the market's fall over the hour dominates.
- **Latency matters less here.**
  - 50 ms costs the market maker fills and, by missing some of those fills, reduces its loss.
  - The taker's results don't depend on the queue model at all, as expected: it never rests an order.
- **For the strategy itself:** the market maker loses money under every assumption, so the verdict doesn't depend on them. The ranking of the queue models is the same at every latency.

Before the study, Avellaneda–Stoikov's quotes always sat one tick behind the best, because of how `min_spread_ticks` was applied. Fills then came only when the market traded through a whole level, and the queue model changed nothing but how fills were split. Fixed, with a test.
