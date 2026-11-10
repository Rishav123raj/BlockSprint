# Developer setup: the Python module

`uv sync` builds the C++ simulator into a Python extension, `blocksprint_sim` (Phase 4 of [blocksprint_2.0_implementation_plan.md](../blocksprint_2.0_implementation_plan.md)). This page covers what that needs and the edit–build–test loop.

## Requirements

| | Windows | Linux |
| :--- | :--- | :--- |
| Compiler | Visual Studio 2022 with "Desktop development with C++" (MSVC). The x64 Native Tools prompt is **not** needed: CMake finds Visual Studio itself | GCC 9+ or Clang 10+ |
| CMake | 3.15 or later, on `PATH` (if missing or too old, scikit-build-core downloads one) | same |
| Python | 3.13 (`uv` installs it if needed) | same |
| uv | 0.8 or later | same |

Everything else is in the repository:
- **pybind11 2.13.6:** vendored under `external/pybind11`, headers plus CMake support, no download at build time.
- **scikit-build-core 0.10.7:** pinned in `pyproject.toml`'s `[build-system]`; uv installs it in an isolated build environment.

## First build

```powershell
uv sync
```

The first build takes 2–3 minutes. It compiles the engine, the simulator and the module, then installs the project in editable mode: `research/` is used from the source tree, and the compiled `blocksprint_sim` goes into `.venv`. Check it:

```powershell
uv run python -c "import blocksprint_sim as bs; print(bs.__version__, bs.strategy_names())"
```

## The loop

| You changed | Do | Time |
| :--- | :--- | :--- |
| Python only (`research/`, tests) | nothing: editable install | 0 |
| C++ (`src/**`) or `CMakeLists.txt` | `uv sync` (or just `uv run ...`, which syncs first) | ~40 s incremental |
| nothing | `uv sync` is a no-op | 0.2 s |

Two mechanisms make the C++ rows work:
- `[tool.uv] cache-keys` in `pyproject.toml` lists the C++ sources, so uv notices a C++ change and rebuilds.
- The build directory `build-py/<wheel tag>/` is kept between builds (git-ignored), so only changed files recompile.

The plain CMake build (`cmake -B build`, `sim_tests`, `bs_sim`) is separate and unchanged. The module target is off there unless you pass `-DBLOCKSPRINT_BUILD_PYTHON=ON`.

**Don't let a rebuild touch the recorder.** The recorder's supervisor runs `uv run --no-sync`, so restarting it never rebuilds or reinstalls the project (`scripts/run_recorder.ps1`). Use `--no-sync` for any long-running job that must not wait for a C++ build.

## Using it

```python
import blocksprint_sim as bs

meta = bs.run({
    "dataset": "data/packed/binance_spot_btc_eth_1105_1107",   # a dataset directory or a .bse file
    "symbol": "BTCUSDT",
    "window": {"from": "2026-11-05T02:00:00Z", "to": "2026-11-05T03:00:00Z"},
    "latency_preset": "binance_spot_tokyo_vps",
    "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 500}},
    "output": {"dir": "results/py_demo"},
}, overrides=["seed=7"])
print(meta["output_hash"], meta["sessions"][0]["exchange_account"]["pnl"])
```

- `bs.run` is `bs_sim run`. It takes the same config format (`docs/simulator_architecture.md` §13.1) and the same `--set` overrides, writes the same results directory, and returns `run_meta.json` as a dict. The Python interpreter lock is released while it runs.
- `bs.resolve_config(config, overrides)` returns the resolved config, as `bs_sim config` prints it.
- Errors in a config (an unknown key, strategy or parameter) raise `ValueError` naming the key.

### A strategy in Python (P4.3, P4.4, P4.6)

```python
import blocksprint_sim as bs
from research.strategies import Context, ExecReport, Strategy

class TimerQuoter(Strategy):
    """Every second: a post-only bid at the best bid."""
    callback_mode = "timer_only"                         # used unless the caller passes one

    def on_start(self, ctx: Context) -> None:
        ctx.set_timer(1_000_000_000)                     # ns

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        book = ctx.book()
        if ctx.can_trade() and book.valid() and (bid := book.best_bid()):
            ctx.send(bs.Side.BUY, bs.OrderType.LIMIT_MAKER, bid.price, bs.to_fixed("0.001"))
        ctx.set_timer(1_000_000_000)

    def on_exec(self, ctx: Context, report: ExecReport) -> None:
        if report.type == bs.ExecType.FILL:
            print(bs.from_fixed(report.last_qty), "@", bs.from_fixed(report.last_px))

cfg = {"dataset": "...", "latency_preset": "binance_spot_tokyo_vps", "strategy": {"name": "null"},
       "output": {"dir": "results/py_quoter"}}
res = TimerQuoter().run(cfg)                             # Results (below); or bs.Simulator(bs.SimConfig(cfg), TimerQuoter()).run()
```

**The base class** (`research/strategies/base.py`):
- **Callbacks:** override the ones you need. The defaults are marked so the C++ adapter never calls them, so they cost nothing.
- **Class attributes:** `name` (default: the class name) and `callback_mode` (default `every_event`). `Simulator` and `Results.run` use the strategy's `callback_mode` unless they're given one.
- **Typos fail early:** a method named `on_*` that isn't a callback (for example `on_fill`) raises `TypeError` when the class is defined, instead of silently never running.
- **Types:** `Context`, `Book`, `BookLevel`, `TradeEvent`, `ExecReport`, `RequestReject`, `Account` and `OpenOrder` are typing `Protocol`s describing the C++ objects, for editors and type checkers (mypy passes). They cost nothing at run time. A test checks that each lists exactly the members of its bound class.
- **Plain objects still work:** a strategy needn't subclass anything; any object with some of the methods below runs. `strategy.name` in the config is still required; `"null"` will do.

**A worked example: `research/strategies/passive_quoter.py`.** This is a line-for-line port of the C++ `PassiveQuoter`. `tests/python/test_parity.py` checks it against the C++ strategy (P4.7): given the same config, the two produce the same requests at the same nanoseconds, so all five output files and the output hash are identical.
- **Scenarios:** random latency, three queue models, a warm-up window, plain LIMIT quotes, and a 3 ms refresh that finds requests still in flight.
- **Callback modes:** the result is the same in every mode.
- **Data gap:** the recorded Nov 6 gap is covered by an extra test that needs the local data and takes about 2 minutes, so it runs only with `BLOCKSPRINT_SLOW_TESTS=1`.
- **Keeping the two in step:** a change to one strategy must be made to both.

```python
from research.strategies.passive_quoter import PassiveQuoter
res = PassiveQuoter.from_params({"size": "0.001", "refresh_ms": 500}).run(cfg)
```

**Callbacks.** A strategy has some of these methods; a missing method costs nothing:

| Method | When |
| :--- | :--- |
| `on_start(ctx)` | at the start of the window |
| `on_book(ctx, book)` | a depth update or snapshot reached the strategy |
| `on_trade(ctx, trade)` | a data trade reached the strategy |
| `on_exec(ctx, report)` | an execution report for its order |
| `on_reject(ctx, reject)` | the exchange refused a cancel, amend or replace |
| `on_timer(ctx, timer_id)` | a timer it set |
| `on_gap(ctx)` | data was lost; the book is invalid until the next snapshot |
| `on_end(ctx)` | at the end of the window |

**Callback modes** filter `on_book` and `on_trade` in C++, before any Python runs:
- `every_event`, the default, delivers both for every update;
- `on_change` calls `on_book` only when the best bid or ask (price or quantity) changed, and delivers every trade;
- `timer_only` delivers neither; read `ctx.book()` in `on_timer` instead.

**Units.** Prices and quantities are integers in 1e-8 units, exactly as in C++. `bs.to_fixed("100.01")` gives `10001000000` and `bs.from_fixed(...)` turns it back into a decimal string. Strategies stay exact and can match a C++ strategy bit for bit.

**The GIL.** It is released for the whole replay and taken only to call a method the strategy has, so other Python threads keep running.

**Errors.**
- An exception in a callback ends the run and comes out of `run()` unchanged.
- Sending during the warm-up raises `RuntimeError`; check `ctx.can_trade()`.
- The `ctx` and `book` objects are valid only during the callback, so don't keep them.

**Speed** (BTCUSDT 2026-11-06, 26.8 M records, i5-6300U, `TimerQuoter` above with a 1 s timer):

| Mode | Time | Python callbacks |
| :--- | ---: | ---: |
| `timer_only` | 31.9 s | 234 k |
| `on_change` | 37.9 s | 3.54 M |
| `every_event` | 38.2 s | 3.69 M |

`bs.Simulator(cfg)` without a strategy runs the config's C++ `strategy.name`, the same as `bs.run`.

### Results as numpy and polars (P4.5)

```python
from research.sim.results import Results

res = Results.run(cfg, TimerQuoter(), callback_mode="timer_only")   # or Results.from_simulator(sim) after sim.run()
fills = res.exec_reports[res.exec_reports["type"] == res.codes["type"].index("fill")]
print(fills["last_qty"].sum())                   # numpy, 1e-8 units

frames = res.to_polars()                         # {"orders", "exec_reports", "request_rejects", "bbo_samples", "positions"}
frames["positions"].select("ts_ns", "pnl")
```

**numpy.** `res.orders`, `res.exec_reports`, `res.request_rejects`, `res.bbo_samples` and `res.positions` are structured arrays that view the simulator's own rows, with no copy. They stay valid after the `Simulator` is gone.
- Units are raw: times in ns, prices, quantities and money in 1e-8, enums as codes (`res.codes["side"]` names them).
- The `owner` column indexes `res.owners`, `order` indexes `res.order_ids`, and refused requests' texts are in `res.reject_texts`.
- A field the CSV leaves empty holds 0. The rules are in `src/sim/recorder.hpp`; for example, `last_px` is set only on fills, and a book side with price 0 has no level.
- The low-level call is `sim.tables()`, a dict of the same arrays.

**polars.** `res.to_polars()` returns the five CSV tables, value for value (tested against the files):
- the same columns in the same order;
- the CSV's strings for enums, owners and order IDs;
- nulls where the CSV is empty;
- `*_ns` columns as UTC datetimes;
- fixed-point columns as exact `Decimal(38, 8)`.

Pass `fixed="float"` for floats (plotting) or `fixed="int"` for the raw integers, or `to_polars("positions")` for one table.

**Cost** (BTCUSDT Nov 6, the C++ `PassiveQuoter`, 1 s samples): 365 k rows and 38.6 MB of tables. Keeping them adds about 4% to the run (73.1 s against 70.0 s), and the output hash is unchanged. `to_polars()` takes 1.8 s with decimals and 0.3 s with floats.

## Troubleshooting

| Symptom | Fix |
| :--- | :--- |
| `No CMAKE_CXX_COMPILER could be found` (Windows) | install Visual Studio 2022's C++ workload |
| `Could NOT find Python ... Development.Module` | use a Python with headers: uv's managed Pythons have them; on Linux system Pythons install `python3-dev` |
| a C++ change doesn't show up in Python | `uv sync` (a bare `python` doesn't rebuild); to force a clean build, delete `build-py/` |
| `ZoneInfoNotFoundError: 'No time zone found with key UTC'` | `tzdata` is missing from `.venv`: `uv sync` (it's a dependency) |
| `uv sync` removed a package you installed by hand | uv keeps `.venv` exactly as `pyproject.toml` says: add the package there (`uv add NAME`) |
