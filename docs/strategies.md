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
  - Each is kept at least `min_spread_ticks` from the mid and moved inside the opposite best if it would cross. They are post-only.
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
| Avellaneda–Stoikov, defaults | 1,398 maker | −36.57 | −0.71 | −3.43 | 32.43 | −1.29 |
| Imbalance taker, defaults | 2,419 taker | −178.51 | −0.01 | +1.30 | 179.80 | +0.04 |

- **Executions:** the three buy the same 0.5 BTC. For them the inventory column is mostly the market's move while they bought, not a cost of the method. Done faster, POV bought earlier into a falling price. The passive TWAP's resting bids were filled when the price fell through them: adverse selection, as its −1.23 bps markout shows.
- **Avellaneda–Stoikov:** it keeps the inventory small; the inventory part is only −3.43.
- **Imbalance taker:** with the defaults, the imbalance predicts nothing at 5 s (+0.04 bps), so the taker fees are the whole loss.

`research/experiments/configs/mm_btc.yaml` sweeps Avellaneda–Stoikov's γ × k over the same hour. Higher γ quotes wider and leans harder against the inventory: fills fall from about 1,400 (γ 0.0005) to about 520 (γ 0.05), and the loss from about −36.5 to about −15. k matters little at these values.
