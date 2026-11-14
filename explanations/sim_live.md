# File: src/sim/live_feed.hpp & src/sim/live_feed.cpp

`LiveFeed` (P7.1) is the live server's market data, built with the simulator's own `Synthesizer` ([sim_synthesizer.md](sim_synthesizer.md)). A connector streams normalized L2 events to the server's `/ws/market-feed` ([api_server.md](api_server.md)); each becomes the same `.bse` records the replayer reads from a file, and goes through the same Synthesizer on the engine's clock (the wall clock in the server). **Paper trading and the simulator share one L2 → L3 path:** synthetic FIFO queues, unexplained cancels placed by the queue model, and trades as tape IOCs that fill a client's order when it is ahead in the queue. Design: [docs/live_platform_architecture.md](../docs/live_platform_architecture.md).

---

## Messages

JSON objects, or an array of them in one frame. Prices and quantities are decimal strings (or numbers) on the symbol's tick and lot grid; `ts` (optional) is the exchange's time in ns.

| Message | Meaning |
| :--- | :--- |
| `{"type": "snapshot", "symbol", "bids": [[p, q], …], "asks": […]}` | The whole book |
| `{"type": "depth", "symbol", "bids", "asks"}` | Absolute quantities per level; 0 removes the level |
| `{"type": "trade", "symbol", "price", "qty", "side"}` | `side` is the aggressor's: `buy` or `sell` |
| `{"type": "gap", "symbol"}` | Updates were lost |

Until a symbol's first snapshot, and after a gap until the next one, depth is ignored and trades aren't matched, as in a replay. Ordering and gap detection are the connector's job (`research/live/feed_bridge.py`, [research.md](research.md)).

## What it does

- **Per symbol** it keeps the symbol's spec (which must be set in the engine before the first message), a queue model, an `Rng` on the stream `queue/<SYMBOL>`, and a `Synthesizer`.
- **`apply(message)`** converts prices and quantities to ticks and lots, builds the records into a batch and hands it to the Synthesizer. A malformed message, an unknown symbol or a value off the grid throws `std::invalid_argument` (with its index in an array); the messages before it have been applied.
- **`client_trades(trades)`**: the server passes every client order's taker trades in, for the impact overlay.
- **`take_gaps()`** returns the symbols that had a gap since the last call; the server tells `/ws/market-data` subscribers.
- **`stats()`** (`GET /api/market-feed`): per symbol, `synced`, message and batch counts, gaps, and fidelity. `trade_reproduced` is the share of synced trades whose tape IOC took exactly the trade's quantity of synthetic liquidity; client orders ahead in the queue lower it, since those are their passive fills. Also the diverted, traded-through and short quantities, crossing fills, and the data's top of book.
- **`reset()`** forgets every symbol after an engine reset; each needs a snapshot again.

`LiveFeedOptions` (`queue_model`, `seed`, `impact_half_life_ns`) come from the server's `--feed-*` options ([main.md](main.md)).

`tests/sim/live_feed_test.cpp` checks that the same events give the same book live and in a replay; `tests/python/test_market_feed.py` drives a real server.
