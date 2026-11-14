# Explanations

One page per source file or component: what it does, its key types, and the rules it follows. Design documents with the reasoning behind them are in [docs/](../docs/); these pages map the code.

## Core engine (`src/core`, `src/api`, `src/main.cpp`)

| Page | Covers |
| :--- | :--- |
| [types.md](types.md) | Fixed-point prices and quantities (1e-8 units) |
| [models.md](models.md) | `Order`, `Trade`, order types, sides, self-trade prevention |
| [events.md](events.md) | Execution reports: the lifecycle contract and every reason |
| [order_book.md](order_book.md) | One symbol's book: price-time matching, order types, amend, validation |
| [matching_engine.md](matching_engine.md) | Routes to books; callbacks; options for the simulator |
| [symbol_spec.md](symbol_spec.md) | Tick, lot, quantity range and minimum notional |
| [fees.md](fees.md) | Maker/taker schedules and exact fee rounding |
| [clock.md](clock.md) | Wall clock and simulation clock |
| [market_data.md](market_data.md) | BBO and depth structs |
| [logger.md](logger.md) | The log |
| [api_server.md](api_server.md) | REST and WebSocket server, including `/ws/market-feed` |
| [main.md](main.md) | The live server's entry point and options |
| [tests.md](tests.md) | Core tests and benchmarks |

## Simulator (`src/sim`)

| Page | Covers |
| :--- | :--- |
| [sim_data_files.md](sim_data_files.md) | The `.bse` format, memory mapping, the replayer, the L2 book, reconstruction and slices |
| [sim_synthesizer.md](sim_synthesizer.md) | L2 → L3: synthetic FIFO queues, queue models, impact |
| [sim_simulator.md](sim_simulator.md) | The event loop, deterministic random numbers, latency, the gateway, the observed book, the result files |
| [sim_strategies.md](sim_strategies.md) | The strategy interface, `RiskGuard`, the C++ reference strategies |
| [sim_run.md](sim_run.md) | The run config, `run_simulation` and the `bs_sim` command line |
| [sim_live.md](sim_live.md) | `LiveFeed`: the live server's book built by the simulator's Synthesizer |

## Python

| Page | Covers |
| :--- | :--- |
| [bindings.md](bindings.md) | `blocksprint_sim`: runs, Python strategies, zero-copy results |
| [research.md](research.md) | The `research/` toolkit: data, strategies, experiments, analytics, paper mode, calibration |

## Reading order

New to the code: [types.md](types.md) → [order_book.md](order_book.md) → [events.md](events.md) → [sim_synthesizer.md](sim_synthesizer.md) → [sim_simulator.md](sim_simulator.md) → [sim_strategies.md](sim_strategies.md) → [research.md](research.md).
