# Experiments: YAML to stored, cached runs

`python -m research.run` turns an experiment file into simulator runs (Phase 5, P5.1–P5.3). It runs them in parallel and keeps each run's results in a store, where a run that was already done is reused, not repeated.

```powershell
uv run python -m research.run research/experiments/configs/quoter_fixture.yaml --dry-run   # list the runs
uv run python -m research.run research/experiments/configs/quoter_fixture.yaml             # run them
```

```text
[ 1/12] ran    2f23c52ce4799465  latency_preset=zero queue_model.model=cancel_from_back seed=2  2.7 s  pnl -1.56701318 fills 97
...
quoter_fixture: 12 runs in 6.3 s (12 ran); summary results\experiments\quoter_fixture\summary.json
```

Running it again lists the 12 runs as `cached`, with identical numbers, in under a second.

After the runs it writes a tearsheet per run and a comparison report (`results/experiments/<name>/report.html`; P5.9, [analytics.md](analytics.md)). `--no-report` skips them.

## The file

```yaml
name: quoter_latency                  # letters, digits, _ . -
description: optional text
dataset: data/packed/binance_spot_btc_eth_1105_1107    # a dataset directory or a .bse file
symbol: BTCUSDT
window: {from: "2026-11-05T02:00:00Z", to: "2026-11-05T03:00:00Z", warmup_s: 60}
seeds: [1, 2, 3]                      # each point runs once per seed (default [0])
latency_preset: binance_spot_tokyo_vps   # or latency: {feed: ..., entry: ..., ack: ...}
queue_model: {model: cancel_from_back}
fees: {maker_rate: "0.001", taker_rate: "0.001"}
sample_ms: 1000
strategy:
  name: passive_quoter                # a C++ strategy, or:
  # python: research.strategies.passive_quoter:PassiveQuoter
  # callback_mode: timer_only         # Python only; default: the class's own
  params: {size: "0.001", refresh_ms: 500}
sweep:
  mode: grid                          # every combination; or random with samples: N and seed: S
  params:
    latency_preset: [zero, binance_spot_tokyo_vps, binance_spot_home_pc]
    queue_model: [{model: cancel_from_back}, {model: pro_rata}, {model: prob_power, n: 2}]
    strategy.params.refresh_ms: [100, 500]
results: results                      # the store's root
```

- **Simulator settings** are the run-config format of `bs_sim` (`docs/simulator_architecture.md` §13.1): `dataset`, `symbol`, `window`, `tie_break`, `latency_preset`, `latency`, `latency_regime`, `queue_model`, `impact`, `fees`, `limits`, `risk` and `gap_policy`. Unknown top-level keys are errors.
- **Sweep keys** are dotted paths into the run config, as `bs_sim run --set` takes them. `strategy.params.X` sets a strategy parameter, for C++ and Python strategies alike.
  - A value replaces whatever is at its key.
  - To sweep queue models that take parameters, sweep whole `queue_model` objects. Sweeping `queue_model.model` would keep the base's `n`, and `pro_rata` refuses an `n`.
- **Random sweeps** (`mode: random`, `samples`, `seed`): a value is a list to choose from, or `{uniform: [a, b]}`, `{log_uniform: [a, b]}` or `{int: [a, b]}`. The draws are reproducible from the sweep's `seed`.
- **Everything is checked when the file is loaded.** Every run of the plan goes through the C++ config resolver, including the C++ strategy's parameters, and a Python strategy is built once with its parameters. An error names the point, its values and the key, before anything runs (exit code 2).
- **Paths** (`dataset`, `results`) are relative to the working directory, as for `bs_sim`.

## Suites and labels

**Suites:** a file can hold several experiments that share settings. It has `defaults`, which are merged into each entry of `experiments` key by key, and an optional `description`. `python -m research.run` runs each experiment in turn, with its own summary and report; `--dry-run` lists all of them. Names must differ. An error names the entry, e.g. `experiments[1] (twap): ...`. From Python, use `Experiment.load_all(path)` for a suite; `Experiment.load` refuses one.

```yaml
defaults:
  dataset: data/packed/binance_spot_btc_eth_1110
  sweep: {params: {latency_preset: [zero, binance_spot_tokyo_vps]}}
experiments:
  - {name: quoter, strategy: {name: passive_quoter}}
  - {name: mm, strategy: {python: research.strategies.avellaneda_stoikov:AvellanedaStoikov}}
```

**Labels:** `sweep.labels` names a key's values, one label per value in the same order. It is for values that are objects, such as whole latency models:

```yaml
sweep:
  params:
    latency: [{feed: {model: constant, us: 0}, ...}, {feed: {model: constant, us: 5000}, ...}]
  labels:
    latency: ["0 ms", "5 ms"]
```

Labels appear in the progress lines, the summary (`labels` per run, `label.*` columns in `ExperimentResult.to_polars()`) and the reports. They don't change a run's identity.

`research/experiments/configs/sensitivity.yaml` (P5.12) is a suite: latency × queue model for each reference strategy ([strategies.md](strategies.md)).

## The store

```text
results/<run_id>/
    run_meta.json                                       the simulator's metadata + run_id, identity, experiment point, git
    orders.parquet  exec_reports.parquet  request_rejects.parquet  bbo_samples.parquet  positions.parquet
results/experiments/<name>/summary.json                 every run of the last invocation
```

- **Tables:** the Parquet files are `Results.to_polars()`, equal to the simulator's CSVs, with exact `Decimal(38, 8)` values and UTC datetimes. The CSVs aren't kept; their `output_hash` is in `run_meta.json`.
- **`run_id`** is the first 16 hex digits of the SHA-256 of the run's identity:
  - **config:** the resolved run config without `output.dir` and without the dataset path, so the same data under another path is the same run;
  - **data:** the `.bse` file's SHA-256, from the dataset manifest, else hashed;
  - **engine:** the SHA-256 of the compiled `blocksprint_sim` module, so rebuilding the engine makes new runs;
  - **Python strategy:** its class, parameters and callback mode, plus the SHA-256 of its source file and of `research/strategies/base.py`.
- **Why not the git SHA:** the plan named the git SHA for the engine. Hashing the module and the strategy source also catches uncommitted changes. The commit (and whether the tree was dirty) is still recorded under `git`.
- **Writes are atomic:** a run is written to `results/.tmp-*` and renamed into place when complete. A directory named by a `run_id` is always a whole run.
- **Failed runs aren't stored,** so they run again next time. `--force` runs everything again.

Reading runs back:

```python
from research.experiments.store import Store
run = Store("results").load("2f23c52ce4799465")
run.meta["sessions"][0]["exchange_account"]["pnl"]
run.frame("exec_reports")                # polars
```

From Python, `run_experiment(Experiment.load(path), workers=4)` returns an `ExperimentResult`: `.runs` (one record per run), `.to_polars()` (the summary as a table) and `.load(record)` (a stored run).

## Running

- **Workers:** runs that aren't in the store run in a `ProcessPoolExecutor` with one simulator per process. The default is one worker per CPU; `--workers 1` runs in the calling process.
- **Failure isolation:**
  - **An exception in a run** (say, a strategy bug) fails that run, with its error in the progress line and the summary; the others carry on.
  - **A crashed worker process** breaks the pool. Every unfinished run is then retried alone in its own process, so only the run that crashes fails. A test kills workers with `os._exit` to check this.
- **Exit code:** `research.run` exits 1 if any run failed.
- **Speed:** on the i5-6300U (2 cores, 4 threads, recorder running), 4 one-hour BTCUSDT quoter runs take 23.2 s with 1 worker and 13.8 s with 4.
