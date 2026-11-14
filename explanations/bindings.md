# File: src/bindings/pybind_module.cpp

`blocksprint_sim`, the simulator as a Python module (Phase 4). It is built with pybind11 through scikit-build-core: `uv sync` compiles it into the virtual environment (`pyproject.toml` turns on the CMake option `BLOCKSPRINT_BUILD_PYTHON`). Setup: [docs/dev_setup.md](../docs/dev_setup.md).

---

## What it exposes

| Name | What it is |
| :--- | :--- |
| `run(config, overrides=[])` | One run, exactly as `bs_sim run` (P4.1). `config` is a dict or JSON text in the run-config format ([sim_run.md](sim_run.md)); returns `run_meta.json` as a dict |
| `resolve_config(config, overrides=[])` | The resolved config, as `bs_sim config` prints it |
| `SimConfig(config, overrides=[])` | A resolved config (P4.3). The C++ strategy's name and params are checked at construction |
| `Simulator(config, strategy=None, callback_mode=None)` | One run with the config's C++ strategy, or a **Python strategy** (P4.4). `run()` writes the results directory; `tables()` returns the outputs |
| `Context`, `ObservedBook`, `BookLevel`, `TradeEvent`, `ExecReport`, `RequestReject`, `Account`, `OpenOrder` | The objects a Python strategy receives ([sim_strategies.md](sim_strategies.md)) |
| `Side`, `OrderType`, `StpPolicy`, `ExecType`, `ExecReason`, `Liquidity`, `OrderStatus`, `Request` | The C++ enums |
| `to_fixed("100.01")`, `from_fixed(n)`, `FIXED_SCALE` | Decimal ↔ integer in 1e-8 units. Prices and quantities are integers, as in C++: exact |
| `data_bbo`, `data_trades`, `data_level_qty` | The data's BBO timeline, trades and level quantities from a `.bse` file, as numpy arrays (for analytics, [sim_data_files.md](sim_data_files.md)) |
| `strategy_names()`, `latency_presets()`, `result_codes()` | What `strategy.name` and `latency_preset` accept; the enum codes in `tables()` with their CSV strings |

C++ `std::invalid_argument` (bad config keys, unknown strategy or parameter) arrives as `ValueError`; `std::logic_error` (trading during the warm-up) as `RuntimeError`.

## Python strategies (P4.4)

Any Python object with some of `on_start`, `on_book`, `on_trade`, `on_exec`, `on_reject`, `on_timer`, `on_gap`, `on_end` is driven by a C++ adapter. **The GIL is released for the whole replay** and taken only to call a method the strategy defines. `callback_mode` filters in C++ before the GIL is taken:

| Mode | Callbacks |
| :--- | :--- |
| `every_event` | `on_book` for every depth event and snapshot, `on_trade` for every trade |
| `on_change` | `on_book` only when the best bid or ask (price or quantity) changed; every trade |
| `timer_only` | No `on_book`/`on_trade` (the book is still kept: read `ctx.book()` in `on_timer`) |

The other callbacks are always delivered. The `Context` and `ObservedBook` passed to a callback are valid during that callback only. `research.strategies.Strategy` is the base class to use: it skips callbacks a subclass doesn't override, and rejects a misspelt `on_*` method when the class is defined ([research.md](research.md)).

## Results without copies (P4.5)

`Simulator.tables()` returns the run's five tables as **numpy structured arrays that view the recorder's rows** (each array keeps them alive), plus `owners`, `order_ids` and `reject_texts`, which the integer columns index. Units are raw: ns, 1e-8 fixed point and enum codes. `research.sim.results.Results` wraps them and converts to polars DataFrames that equal the CSVs.

`tests/python/test_bindings.py` (values, configs, runs, the pinned hashes), `test_python_strategies.py` (callback modes, a Python null strategy matching the C++ one, the GIL released during the replay) and `test_results.py` cover the module.
