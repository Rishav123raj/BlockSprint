# File: src/core/types.hpp & src/core/types.cpp

Fixed-point numbers for prices and quantities (2.0, P1.6). Every price and quantity in the engine is an `int64_t` in units of **1e-8**, the finest precision Binance uses for any spot symbol. All matching arithmetic is exact integer arithmetic.

1.0 used `double`. Prices like `0.1 + 0.2` could then differ in the last bit, so one price could become two separate levels in the book (defect D5).

---

## What it Does

| Name | Meaning |
| :--- | :--- |
| `Price`, `Quantity` | `int64_t`, in 1e-8 units: `100.5` is `10050000000` |
| `kFixedDecimals`, `kFixedScale` | 8 and `100000000` |
| `parse_fixed(text)` | Exact decimal string → units. Accepts an optional `+`, digits with at most one `.`, and an optional exponent (`"1e-05"`). Returns `nullopt` for malformed or negative text, out-of-range values, and anything finer than 1e-8 (`"0.000000001"`), unless the extra digits are zeros (`"1.000000000"` is fine) |
| `format_fixed(units)` | Exact decimal with trailing zeros trimmed and at least one decimal: `"101.5"`, `"7.0"`, `"0.00000001"`. Used by execution reports and error messages |
| `format_legacy(units)` | The 1.0 wire format, `std::to_string` of the double (`"100.500000"`). Kept for the trade, BBO and depth JSON so existing clients keep working |
| `to_double`, `from_double` | Lossy conversions for display, tests and Python callers. `from_double` rounds to the nearest unit |

The API parses every price, quantity and rate with `parse_fixed`, whether the client sends a string (`"0.5"`) or a number (`0.5`), so a value is never rounded on the way in.

## Ranges

`int64` in 1e-8 units holds values up to about 92 billion. Products of two fixed-point values (notional = price × quantity, fee = notional × rate) need 128 bits in between; `int128.hpp` provides a small unsigned 128-bit multiply and divide for that (MSVC has no `__int128`), used by the minimum-notional check ([symbol_spec.md](symbol_spec.md)) and by fees ([fees.md](fees.md)).

`tests/core/types_test.cpp` covers parsing, formatting, exponents, range limits and the D5 regression.
