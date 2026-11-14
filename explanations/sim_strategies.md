# Files: src/sim/strategy.hpp, risk_guard, strategies/

The strategy interface, the strategy's own pre-trade risk limits, and the C++ reference strategies. Python strategies use the same interface through the bindings ([bindings.md](bindings.md), [research.md](research.md)); the catalogue of strategies is in [docs/strategies.md](../docs/strategies.md).

---

## The interface (`strategy.hpp`, P3.10)

A strategy reacts to callbacks and acts through its `Context`. **Everything it sees arrives with latency:** the book after feed latency, reports after ack latency, and its requests reach the exchange after entry latency ([sim_simulator.md](sim_simulator.md)). The same interface runs unchanged in paper mode (`research/live/runner.py`, [research.md](research.md)).

| Callback | When |
| :--- | :--- |
| `on_start` / `on_end` | The window's start and end |
| `on_book(ctx, book)` | A depth event or snapshot reached the strategy |
| `on_trade(ctx, trade)` | A data trade |
| `on_exec(ctx, report)` | An execution report for one of its orders |
| `on_reject(ctx, reject)` | A cancel, amend or replace the exchange refused |
| `on_timer(ctx, id)` | A timer it set |
| `on_gap(ctx)` | Updates were lost on a symbol's feed; its book is invalid until the next snapshot |

| `Context` call | Meaning |
| :--- | :--- |
| `now()`, `symbol()`, `symbols()`, `owner()` | Sim time; the primary symbol; every symbol (P6.7); the participant ID |
| `book(symbol?)` | The `ObservedBook`: the market as the strategy sees it |
| `can_trade()` | False during the warm-up (requests then throw `std::logic_error`; timers are allowed) |
| `send([symbol,] side, type, price, qty, stp?)` | A new order; returns its ID (`<owner>-<n>`). Validation happens at the exchange. Self-trade prevention defaults to `cancel_maker` |
| `cancel`, `amend(id, new_total, price?)`, `replace` | Act on the order's own symbol. `amend` takes the new **total** (filled + open), as on Binance; `replace` cancels and re-sends under the same ID, losing priority |
| `account(symbol?)`, `open_orders()` | As **known to the strategy**, from the reports it has received |
| `set_timer(delay)`, `cancel_timer(id)` | Timers fire with no latency |

**`Account`** holds position (base) and cash and fees (quote) in 1e-8 units; `pnl(mark)` is `cash + position × mark − fees`. There are two copies per strategy: the exchange's (the run's results) and the strategy's own, updated only when it learns of a fill. `OpenOrder` is an order as the strategy knows it, with a pending status while a request is in flight.

## `RiskGuard` (P6.4)

Pre-trade limits on a strategy's own requests, checked in its `Context` **before** they enter the latency pipe, against what the strategy knows. Every limit is off at 0.

| Limit | Refuses (reason) |
| :--- | :--- |
| `max_position` | An order that could take \|position\| over the limit if it and every other open order on its side filled, per symbol (`risk_position`) |
| `max_order_notional` | price × quantity over the limit; a MARKET order is priced at the observed best opposite price (`risk_notional`) |
| `max_open_orders` | A new order when the strategy has this many open or pending orders, all symbols (`risk_open_orders`) |
| `max_drawdown` | Kill switch: when PnL at the observed mids falls this far below its running high, every open order is cancelled and from then on only orders that reduce the position without reversing it pass (`risk_killed`) |

A refused request never reaches the exchange; the strategy hears at once, with a `REJECTED` report for a new order or a request reject for an amend or replace ([events.md](events.md)). Cancels always pass, and so does an amend that doesn't increase the quantity. Configured under `risk` in the run config.

## Reference strategies (`strategies/`)

| File | Strategy | Params |
| :--- | :--- | :--- |
| `registry.hpp/.cpp` | Builds a strategy from `strategy.name`: `null` (never trades: a replay with the full loop, for benchmarks and data checks), `passive_quoter`, `twap_taker` | — |
| `passive_quoter.*` (P3.15) | Joins the best bid and ask with one post-only quote per side, re-quoting on a timer. A quote at another price is replaced; a side whose next fill would pass `max_position` isn't quoted; every quote is cancelled while the book is invalid; a quote with a request in flight is left alone. At most 2 requests per refresh | `size` ("0.001"), `refresh_ms` (500), `max_position` ("0.01"), `post_only` (true), `requote_always` (false) |
| `twap_taker.*` (P3.15) | Buys or sells a total in equal MARKET slices over a duration, starting after the warm-up. Each slice sends the target less what is filled or in flight, so anything a slice couldn't fill is retried with the next; after the last slice it keeps retrying the remainder | `side`, `quantity` ("0.01"), `slices` (10), `duration_ms` (600000) |
| `params.hpp` (P3.14) | Typed getters over `strategy.params` with defaults. `done()` rejects keys nobody read, so a misspelt parameter is an error naming the key (`strategy.params.size: ...`) | — |

`research/strategies/passive_quoter.py` is a line-for-line Python port of the passive quoter. `tests/python/test_parity.py` checks that both give the determinism test's pinned output hashes. `tests/sim/strategies_test.cpp` covers the C++ strategies and the registry; `tests/python/test_risk_guard.py` and `test_gap_policy.py` the risk limits and gap policies.
