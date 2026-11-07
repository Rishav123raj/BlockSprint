# File: src/core/fees.hpp & src/core/fees.cpp

Maker/taker fees (2.0, P1.13). Every fill is charged to both sides at their owner's rate and reported in the execution reports, so a backtest's PnL is net of fees.

---

## What it Does

**`FeeSchedule {maker_rate, taker_rate}`:** rates are fixed-point fractions in 1e-8 units: `0.001` (0.10%) is `100000`. A negative rate is a rebate. `binance_spot_regular()` is Binance's regular (VIP 0) spot tier, 0.10% / 0.10%.

**`FeeBook`:** one schedule per owner plus a default for everyone else.

| Call | Meaning |
| :--- | :--- |
| `set_default(schedule)` | The schedule for owners without their own |
| `set(owner, schedule)` / `clear(owner)` | An owner's own schedule |
| `schedule_for(owner)` | The schedule that applies (own, else default). Skips hashing the owner when no owner has a schedule (P1.18) |
| `own_schedule(owner)` | The owner's own schedule, if any |

The engine owns one `FeeBook`, shared by its books (`MatchingEngine::set_default_fee_schedule`, `set_fee_schedule`, or `POST /api/fees`). A new engine charges nothing; the live server starts with `binance_spot_regular()`. Schedules survive `reset()`.

**`notional_for(price, quantity)`:** the quote value of a fill in 1e-8 units, rounded half away from zero (exact whenever the price and quantity decimals add up to 8 or fewer, which covers every Binance spot symbol). The simulator's accounts use it (P3.10).

**`fee_for(price, quantity, rate)`:** the fee for one fill, in quote units of 1e-8, exact. The notional (price × quantity) is rounded to 1e-8, then the fee (notional × rate) is rounded to 1e-8, both half away from zero, using 128-bit intermediates (`int128.hpp`). It is never more than 1e-8 off the unrounded value; notionals above about 92 billion quote units saturate.

## How the book charges fees

For each fill at the maker's price, the taker pays its owner's `taker_rate` and the maker its owner's `maker_rate`. The fee is reported in that side's `FILL` report (`fee`) and accumulated in the order (`fees_paid`, reported as `cum_fee`), which survives amends. Market-data liquidity pays nothing. Fees are in the quote currency (USDT for BTC-USDT): Binance charges a buyer in the base asset unless BNB is used, and the quote value at the fill price is the same amount, directly comparable in PnL.

`tests/core/fees_test.cpp` covers rounding, signs, rebates, saturation and per-owner schedules; the fuzz test checks every fill's fee against a reference ([tests.md](tests.md)).
