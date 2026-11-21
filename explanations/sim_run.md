# Files: src/sim/config, run, main.cpp (`bs_sim`)

How one JSON document becomes a simulation run, and the simulator's command line. Full key reference: [docs/simulator_architecture.md](../docs/simulator_architecture.md) §13.

---

## Run config (`config.hpp/.cpp`, P3.12)

Layers, later ones win:

```
built-in defaults  <  latency preset ("latency_preset")  <  config file  <  --set key.path=value
```

- Objects merge key by key, and an empty object clears (`--set strategy.params={}`).
- A latency or queue model object that names its `model` replaces the previous one whole, so a preset's lognormal parameters don't leak into a file's constant model.
- **Unknown keys are errors**, naming the key, so a typo can't fall back to a default silently.
- The resolved document (every layer applied, times normalized to ns-precision UTC) is recorded in `run_meta.json`.

| Key | Meaning |
| :--- | :--- |
| `dataset`, `symbol` | A packed dataset directory (its `manifest.json` gives the symbol's `.bse` file and SHA-256) or a `.bse` file. `symbol` may be a list for a multi-symbol run, the primary first (P6.7) |
| `window` | `from`, `to` (ns, `YYYY-MM-DD` or ISO-8601 UTC; 0 = the data's ends), `warmup_s` |
| `seed`, `tie_break` | Every random stream derives from the seed; `data_first` or `orders_first` |
| `latency_preset`, `latency` | `feed`, `entry`, `ack` models. A run must choose a preset or models ([sim_simulator.md](sim_simulator.md)) |
| `latency_regime` | `window_ms`, `curve` ([[rate, multiplier], …]), `channels` (P6.3) |
| `queue_model` | `{"model": "prob_power", "n": 2}`, or a string such as `"prob_power:1.5"` ([sim_synthesizer.md](sim_synthesizer.md)) |
| `impact` | `half_life_ms` (P6.1); `permanent_coefficient`, `daily_volatility`, `daily_volume` (P6.6) |
| `fees` | `maker_rate`, `taker_rate` as decimals (default 0.001 / 0.001, Binance's regular tier) |
| `limits` | Binance's request limits (`orders_per_10s`, `orders_per_day`, `open_orders`, `request_weight_per_min`) |
| `risk` | The strategy's `RiskGuard` limits, off at 0 ([sim_strategies.md](sim_strategies.md)) |
| `gap_policy` | `halt` (default), `flatten` or `continue` |
| `strategy` | `name` (from the registry; `null` for a Python strategy), `owner` (default `S1`), `params` |
| `output` | `dir`, `sample_ms` (BBO and position sample interval), `queue_positions` (optional, default off: also write `queue_positions.csv`, each resting strategy order's queue ahead and behind over time; [simulator_architecture.md §12](../docs/simulator_architecture.md#12-outputs)) |

`resolve_run_config(json, overrides)` (or `load_run_config(path, overrides)`) returns a `RunConfig`: the resolved JSON plus the parsed pieces the simulator needs (`SimulatorOptions`, the session's latency models, fees, limits, risk and gap policy, the data files). `latency_preset_names()` lists the presets. Calibrated profiles (`configs/sim/calibrated/*.json`) are ordinary config files to layer in ([docs/calibration.md](../docs/calibration.md)).

## `run_simulation` (`run.hpp/.cpp`, P3.17)

One run from a resolved config, as a library call: builds the strategy from the registry (or takes one given, such as a Python strategy), runs the simulator with the `ResultRecorder` attached, and writes the results directory. `bs_sim run`, the Python module and the determinism test all go through it, so they take exactly the same path. Returns `RunOutcome`: the metadata, the output hash, simulator and session statistics, the final exchange account, wall time and, with `keep_tables`, the rows in memory. A bad strategy name or parameter throws before any data is read.

## `bs_sim` (`main.cpp`, P3.14)

| Command | Does |
| :--- | :--- |
| `bs_sim info FILE.bse` | Header, record and batch counts by kind, segments, time range |
| `bs_sim reconstruct FILE.bse [--top N] [--every K] [--at TIME]… [--out DUMP.csv]` | Replays the book from the data alone and writes checkpoints for gate V0 |
| `bs_sim synth FILE.bse [--model M] [--compare-every K] [--top N] [--check-every K]` | Replays through the synthesizer into the engine with no strategy and compares the engine's book with the data |
| `bs_sim latency FILE.bse [--kind trade\|depth] [--window-s S] [--out VALUES.txt]` | `ts_local − ts_exch` percentiles, raw and above each window's fastest: input for latency models |
| `bs_sim config CONFIG.json [--set K=V]…` | Prints the resolved config |
| `bs_sim run CONFIG.json [--set K=V]… [--out DIR] [--force]` | A full simulation into `output.dir` (or `DIR`); `--force` replaces earlier results there |
| `bs_sim slice FILE.bse --from T --to T --out OUT.bse` | A self-contained piece of a file: test fixtures and the sample dataset |

```
./build/Release/bs_sim run configs/sim/sample_quoter.json --set seed=2 --out results/q2
```

`tests/sim/config_test.cpp` covers layering, overrides, unknown keys and time formats.
