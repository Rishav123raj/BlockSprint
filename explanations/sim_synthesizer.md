# Files: src/sim/synthesizer, queue_models.hpp, impact

How L2 market data (quantity per price) becomes L3 orders in the matching engine's FIFO book, so a strategy's order gets a realistic queue position and **the engine's own matching produces every fill**. The design and its assumptions are in [docs/simulator_architecture.md](../docs/simulator_architecture.md) §8–9 and §19.

---

## `Synthesizer`

Synthetic orders are owned by `MARKET` and flagged `market_data_liquidity`: self-trade prevention skips them, they pay no fees, and symbol rules don't apply to them ([order_book.md](order_book.md)). Each data level's synthetic quantity is driven towards the data's **absolute** quantity:

| Data event | What the synthesizer does |
| :--- | :--- |
| Trade of q at p | Sends a **tape IOC** (owner `MARKET_TAPE`) of q at limit p from the aggressor's side. It consumes the queue front first, including strategy orders that are ahead. Whatever strategy orders took is then removed from the front of the synthetic queue at p, since the real trade consumed q of real liquidity there |
| Depth / snapshot | Per changed level: synthetic > data, the excess is an **unexplained cancel** and the queue model chooses where it comes from; synthetic < data, the difference joins the back. Decreases go first, so the synthetic book never crosses itself |
| Gap | Every synthetic order is removed. Until the next snapshot trades are skipped and depth is ignored. Strategy orders stay |

Some consequences:
- **Trades are netted against depth by construction.** A trade has already removed its quantity when the depth event reflecting it arrives, so only the remainder (real cancels) goes to the queue model.
- **Crossing fill:** a synthetic order added at a price that crosses a resting strategy order trades with it at the strategy order's price, as the real market would have.
- Adjacent synthetic orders with no strategy order between them are merged; no queue model can tell them apart.

`check(exact)` returns one message per violation: no synthetic quantity where the data has none, never more than the data, and (with `exact`) equality when no strategy traded. `stats()` counts batches, cancels and the V4 gate (each data trade of q should take exactly q of synthetic liquidity when no strategy runs). The same class drives the live server's book through `LiveFeed` ([sim_live.md](sim_live.md)).

## Queue models (`queue_models.hpp`, P3.7)

When a level shrinks and no trade explains it, which part of the queue was cancelled? That decides how fast a strategy order reaches the front.

| Model | Cancels |
| :--- | :--- |
| `cancel_from_back` | Newest synthetic quantity first: the queue ahead only shrinks through trades (pessimistic; the default) |
| `cancel_from_front` | Oldest first (optimistic) |
| `pro_rata` | Every synthetic order in proportion to its size |
| `prob_power:n` | The part ahead of the first strategy order loses `aⁿ / (aⁿ + bⁿ)` of the cancel, the part behind the rest (a, b: synthetic quantity ahead and behind, in lots). The calibrated value for BTCUSDT is n = 1.5 ([docs/calibration.md](../docs/calibration.md)) |
| `prob_log` | The same with `log(1 + x)` |

The probabilistic models use the expected split, not a random draw (as hftbacktest does). Within a part, quantity is taken from the back. Without a strategy order in the level they fall back to `cancel_from_back`. `make_queue_model(name)` parses the names above.

## Impact (`impact.hpp`)

**`ImpactOverlay` (P6.1).** The data keeps showing liquidity a strategy's taker fill took, because the real market never saw that fill. The overlay remembers, per data level, how much a strategy took, decaying with half-life τ: `outstanding(t) = Σ qᵢ · 2^(−(t − tᵢ)/τ)`, floored to whole lots. When the data next changes the level, the synthesizer drives it to `data − outstanding`, so liquidity comes back gradually. τ = 0 (the default) disables it: the level returns to the data on its next change. Entries go when the data empties the level, and all of them on a snapshot or gap.

**`PermanentImpact` (P6.6, off by default).** The square-root law for execution research: the net quantity X the strategies took moves the whole market by `sign(X) · Y · σ · √(|X| / V)` (Y the coefficient, σ daily volatility, V daily volume), rounded to whole ticks. It doesn't decay; selling back what was bought brings it back to 0. The synthesizer moves every data price by the shift, and `Batch::shift` records it so strategies see the moved prices too.

`tests/sim/synthesizer_test.cpp` covers each rule and every queue model; `rng_queue_test.cpp` the model arithmetic; `tests/python/test_impact.py` and `test_permanent_impact.py` the impact layers.
