# File: src/core/market_data.hpp

Plain market-data snapshots (2.0, P1.11). The engine and its callbacks work with these structs; JSON is built only at the boundary.

---

## What it Does

| Type | Fields |
| :--- | :--- |
| `Bbo` | `timestamp`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`. An empty side has price and quantity 0. `same_quotes(other)` compares the four quotes and ignores the timestamp |
| `DepthLevel` | `price`, `quantity` (the level's total open quantity) |
| `Depth` | `timestamp`, `bids`, `asks`: aggregated levels, best first |

`bbo_to_json(symbol, bbo)` and `depth_to_json(symbol, depth)` produce the 1.0 wire format (prices and quantities as `"%f"` strings, `"0.0"` for an empty side) used by `GET /api/bbo`, `GET /api/depth` and `/ws/market-data`.

## Why plain structs

Up to P1.10, every order built the BBO as JSON twice (each with an ISO timestamp formatted through `ostringstream`) and compared the two as strings, and every limit order also built a JSON depth snapshot. That cost 15–100 µs per order. Now a BBO change is four integer comparisons (`same_quotes`), and the API server converts a snapshot to JSON only if a client is subscribed, coalescing depth to one update per symbol every 100 ms. See [docs/baseline/bench_core.md](../docs/baseline/bench_core.md).

Snapshots are taken with `MatchingEngine::bbo(symbol)` and `depth(symbol, levels)`, which never create a book: an unknown symbol reads as empty (1.0 created one, D18).
