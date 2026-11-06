# File: src/core/symbol_spec.hpp & src/core/symbol_spec.cpp

Trading rules per symbol and order validation (2.0, P1.12). The rules mirror Binance's filters, so an order the engine accepts is one Binance would accept too.

---

## What it Does

**`SymbolSpec`** (all values fixed-point, 1e-8 units; 0 means "no rule"):

| Field | Binance filter | Rule |
| :--- | :--- | :--- |
| `tick_size` | `PRICE_FILTER.tickSize` | Price is a multiple of it |
| `lot_size` | `LOT_SIZE.stepSize` | Quantity is a multiple of it |
| `min_qty`, `max_qty` | `LOT_SIZE.minQty` / `maxQty` | Quantity is within the range |
| `min_notional` | `NOTIONAL.minNotional` | Price × quantity, in quote units, is at least this |

Specs are set with `MatchingEngine::set_symbol_spec` or `POST /api/symbols` (localhost only); the Binance connector loads the real filters for the symbols it mirrors. They apply from then on to new orders, amends and modifies, and survive `reset()`.

**`validate_order(order, spec)`** returns the first broken rule as a `ValidationError {reason, text}`, which the book turns into a `REJECTED` report ([events.md](events.md)):

1. Always: quantity > 0 (`invalid_quantity`), and price > 0 for every type but `MARKET` (`invalid_price`; 1.0 turned a missing price into 0, so a sell swept every bid, D6).
2. Only with a spec, and not for market-data liquidity: `price_tick` and `quantity_lot`, then `quantity_range`, then `min_notional`. `MARKET` orders skip the tick and notional checks, because their execution price isn't known on arrival.

Market-data liquidity (mirrored or replayed exchange levels, `Order::market_data_liquidity`) is exempt from the spec because real books hold levels below the minimum notional.

**`notional_at_least(price, quantity, min_notional)`** compares exactly with a 128-bit product (`int128.hpp`), so a notional that would overflow 64 bits is still judged correctly.

`tests/core/validation_test.cpp` covers each rule, the market-data exemption, exact notional comparison, the D6 and D18 regressions, and an invalid modify that leaves the order unchanged. `tests/core/post_only_amend_test.cpp` checks that amends follow the rules too.
