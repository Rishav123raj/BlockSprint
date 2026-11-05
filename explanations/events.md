# File: src/core/events.hpp (Execution Reports, 2.0 P1.10)

Execution reports describe the complete lifecycle of every order, in the order things happened. Earlier versions only returned trades, so a self-trade cancellation, an IOC leftover or a killed FOK simply disappeared. Now each one ends in an explicit report with a reason.

## The contract

* An order the engine refuses gets exactly **one `REJECTED`** report and nothing else.
* An accepted order gets **`ACK`** first (`AMENDED` if `modify` re-submitted it), then any number of `FILL` / `AMENDED` reports, then **exactly one terminal report**: a `FILL` with `leaves_qty == 0`, or a `CANCELLED`.
* An order with no terminal report is resting in the book.
* Every trade produces **two `FILL`s** with the same `trade_id`: the taker's, then the maker's.
* `seq` numbers are consecutive per engine, across all symbols; timestamps come from the engine's clock (`SimClock` in a backtest).
* At every report, `cum_qty + leaves_qty == quantity`, except terminal `CANCELLED` and `REJECTED` reports, where `leaves_qty` is 0.

`tests/core/exec_report_test.cpp` checks all of this on every report of 20 random workloads that use every STP policy. At the end of each run, the orders without a terminal report must be **exactly** the ones still resting.

## Types and reasons

| Type | Reason | When |
| :--- | :--- | :--- |
| `ack` | — | Order accepted |
| `fill` | — | Partial or complete fill: `last_px`, `last_qty`, `liquidity` (`maker`/`taker`), `trade_id`, `fee` |
| `amended` | — | Price or quantity changed by `amend` or `modify` |
| `cancelled` | `user` | Cancel request, or modify to zero quantity |
| | `replaced` | Replaced by a new order with the same ID (`replace_order`) |
| | `ioc_remainder` | Unfilled part of an IOC |
| | `no_liquidity` | Unfilled part of a MARKET order |
| | `fok_kill` | FOK that couldn't fill completely |
| | `would_cross` | LIMIT remainder that would rest crossed after STP skipped own orders (1.0 defect D7) |
| | `stp_taker` | Incoming order cancelled by self-trade prevention |
| | `stp_maker` | Resting order cancelled by an incoming order of the same owner |
| `rejected` | `duplicate_order_id` | An order with the same ID is resting |
| | `invalid_quantity` | Quantity is zero or negative |
| | `invalid_price` | LIMIT / IOC / FOK without a positive price (1.0 defect D6) |
| | `price_tick` | Price isn't a multiple of the symbol's tick size |
| | `quantity_lot` | Quantity isn't a multiple of the symbol's lot size |
| | `quantity_range` | Quantity is below the symbol's minimum or above its maximum |
| | `min_notional` | Limit price × quantity is below the symbol's minimum notional |
| | `post_only_would_take` | A post-only (`limit_maker`) order, or an amend of one, would trade on arrival instead of resting |

The last four apply only to symbols with trading rules (`set_symbol_spec`, `POST /api/symbols`), and not to market-data liquidity (mirrored or replayed exchange levels). An invalid `modify` is also `rejected`; the order stays as it was.

## Fees (P1.13)

Every report carries `cum_fee`, the fees charged to the order so far, and every `fill` carries `fee` for that fill. Fees are in the quote currency (USDT for BTC-USDT): the fill's notional (price × quantity) times the owner's maker or taker rate, rounded to 1e-8. A negative fee is a rebate. Rates come from the engine's fee schedules: one per owner, plus a default (`set_fee_schedule`, `set_default_fee_schedule`, `POST /api/fees`). A new engine charges nothing; the live server defaults to Binance's regular spot tier, 0.10% maker and 0.10% taker. Market-data liquidity (mirrored or replayed exchange levels) pays nothing. Fees appear only in execution reports, never in the public trade feed.

## Getting reports

* **C++:** every mutating `MatchingEngine` call takes an optional `std::vector<ExecReport>* reports`, and `register_exec_report_callback` receives every report before the trade, BBO and depth callbacks.
* **REST / WebSocket orders:** responses carry a `reports` array next to the 1.x `trades` array. A rejected order returns HTTP 400 (WebSocket `"status": "error"`) with the report's `text` as `message`.
* **`/ws/executions`:** a stream of every report, `{"event": "execution", "symbol": ..., "data": {...}}`.

Report JSON uses exact decimals (`"100.5"`, `"0.00000001"`), unlike the 1.x trade and depth format (`"100.500000"`), and only includes fields that apply (`reason`, `text`, `owner` and the fill fields are omitted otherwise).
