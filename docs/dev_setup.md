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

Python strategies, the `Simulator` class and numpy results come in P4.3–P4.6.

## Troubleshooting

| Symptom | Fix |
| :--- | :--- |
| `No CMAKE_CXX_COMPILER could be found` (Windows) | install Visual Studio 2022's C++ workload |
| `Could NOT find Python ... Development.Module` | use a Python with headers: uv's managed Pythons have them; on Linux system Pythons install `python3-dev` |
| a C++ change doesn't show up in Python | `uv sync` (a bare `python` doesn't rebuild); to force a clean build, delete `build-py/` |
| `uv sync` removed a package you installed by hand | uv keeps `.venv` exactly as `pyproject.toml` says: add the package there (`uv add NAME`) |
