"""Experiments (P5.1-P5.3): the YAML schema, the run store and the parallel runner."""

import json
import os
import shutil
from pathlib import Path

import polars as pl
import pytest
import yaml

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.experiments.runner import plan_runs, run_experiment
from research.experiments.schema import Experiment
from research.experiments.store import Store, identity, run_id_of
from research.run import main as run_main
from research.strategies import Context, Strategy

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
SECOND = 1_000_000_000

# tests/sim/determinism_test.cpp, scenario "quoter, random latency, prob_power:2", and its pinned hash
PINNED = {
    "name": "pinned",
    "dataset": str(FIXTURE),
    "seeds": [7],
    "latency": {"feed": {"model": "lognormal", "median_us": 2000, "sigma": 0.5, "min_us": 300},
                "entry": {"model": "normal", "mean_us": 3000, "sd_us": 1000, "min_us": 500},
                "ack": {"model": "lognormal", "median_us": 2500, "sigma": 0.4, "min_us": 400}},
    "queue_model": {"model": "prob_power", "n": 2},
    "sample_ms": 250,
    "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 100, "max_position": "0.005"}},
}
PINNED_HASH = "e4983a252e78434fc0614d479c6e5ef46b05d75758e20f4ab3822b5f380c7728"


def experiment(tmp_path, **changes):
    return Experiment.model_validate({**PINNED, "results": str(tmp_path / "results"), **changes})


def write_yaml(tmp_path, doc):
    p = tmp_path / "exp.yaml"
    p.write_text(yaml.safe_dump(doc), encoding="utf-8")
    return p


# ---- strategies for the runner tests (imported by name in worker processes: this file is on sys.path) ----

class Boom(Strategy):
    """Raises in its first timer when fail is set."""
    callback_mode = "timer_only"

    def __init__(self, fail: bool = False):
        self.fail = fail

    def on_start(self, ctx: Context) -> None:
        ctx.set_timer(SECOND)

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        if self.fail:
            raise RuntimeError("strategy bug")


class Crash(Boom):
    """Kills its process outright (as a native crash would) when fail is set."""

    def on_timer(self, ctx: Context, timer_id: int) -> None:
        if self.fail:
            os._exit(3)


# ---- P5.1 schema ----

def test_a_yaml_experiment_loads_and_plans(tmp_path):
    doc = {**PINNED, "seeds": [1, 2], "results": str(tmp_path),
           "sweep": {"params": {"latency_preset": ["zero", "binance_spot_tokyo_vps"],
                                "strategy.params.refresh_ms": [50, 100, 200]}}}
    del doc["latency"]
    exp = Experiment.load(write_yaml(tmp_path, doc))
    specs = exp.plan()
    assert len(specs) == 2 * 3 * 2
    first = specs[0]
    assert first.params == {"latency_preset": "zero", "strategy.params.refresh_ms": 50} and first.seed == 1
    assert first.config["latency_preset"] == "zero" and first.config["seed"] == 1
    assert first.config["strategy"] == {"name": "passive_quoter",
                                        "params": {"size": "0.001", "refresh_ms": 50, "max_position": "0.005"}}
    assert first.config["output"] == {"sample_ms": 250}
    assert PINNED["strategy"]["params"]["refresh_ms"] == 100  # the base isn't modified


def test_latency_regime_passes_through_and_sweeps(tmp_path):
    doc = {**PINNED, "results": str(tmp_path), "latency_regime": {"window_ms": 500},
           "sweep": {"params": {"latency_regime.curve": [[[0, 1]], [[0, 1], [1000, 3]]]}}}
    specs = Experiment.load(write_yaml(tmp_path, doc)).plan()
    assert [s.config["latency_regime"] for s in specs] == [
        {"window_ms": 500, "curve": [[0, 1]]}, {"window_ms": 500, "curve": [[0, 1], [1000, 3]]}]


def test_random_sweep_is_reproducible_and_in_range(tmp_path):
    sweep = {"mode": "random", "samples": 6, "seed": 4,
             "params": {"strategy.params.refresh_ms": {"log_uniform": [10, 1000]},
                        "queue_model": [{"model": "pro_rata"}, {"model": "prob_power", "n": 3}],
                        "strategy.params.max_position": ["0.002", "0.01"]}}
    a = experiment(tmp_path, sweep=sweep).plan()
    b = experiment(tmp_path, sweep=sweep).plan()
    assert [s.params for s in a] == [s.params for s in b] and len(a) == 6
    assert all(10 <= s.params["strategy.params.refresh_ms"] <= 1000 for s in a)
    assert len({s.params["strategy.params.refresh_ms"] for s in a}) == 6


@pytest.mark.parametrize("change, message", [
    ({"strategy": {"name": "passive_quoter", "python": "x:Y"}}, "exactly one of"),
    ({"strategy": {"name": "nope"}}, "unknown C\\+\\+ strategy"),
    ({"strategy": {"python": "research.strategies.passive_quoter:Nope"}}, "not a class"),
    ({"strategy": {"name": "null", "callback_mode": "timer_only"}}, "Python strategies only"),
    ({"seeds": [1, 1]}, "each seed once"),
    ({"frobnicate": 1}, "frobnicate"),
    ({"sweep": {"params": {}}}, "at least one key"),
    ({"sweep": {"params": {"output.dir": ["a"]}}}, "can't be swept"),
    ({"sweep": {"params": {"seed": {"int": [1, 5]}}}}, "random"),
    ({"sweep": {"mode": "random", "params": {"seed": [1]}}}, "samples"),
])
def test_schema_errors(tmp_path, change, message):
    with pytest.raises(ValueError, match=message):
        experiment(tmp_path, **change).plan()


def test_config_errors_name_the_key_and_the_point(tmp_path):
    # Valid YAML, but one sweep value is wrong: found when the file is loaded, before anything runs
    doc = {**PINNED, "sweep": {"params": {"queue_model": [{"model": "pro_rata"}, {"model": "nope"}]}}}
    with pytest.raises(ValueError, match=r"point 1 .*queue_model"):
        Experiment.load(write_yaml(tmp_path, doc))
    doc = {**PINNED, "strategy": {"name": "passive_quoter", "params": {"sise": "1"}}}
    with pytest.raises(ValueError, match="sise"):
        Experiment.load(write_yaml(tmp_path, doc))
    doc = {**PINNED, "strategy": {"python": "research.strategies.passive_quoter:PassiveQuoter",
                                  "params": {"sise": "1"}}}
    with pytest.raises(ValueError, match="sise"):
        Experiment.load(write_yaml(tmp_path, doc))


# ---- P5.2 store ----

def test_run_identity(tmp_path):
    exp = experiment(tmp_path)
    (spec, run_id, ident), = plan_runs(exp)
    assert len(run_id) == 16 and ident["data_sha256"] and ident["engine_sha256"]
    assert "dataset" not in ident["config"] and "dir" not in ident["config"]["output"]

    # the same data under another path is the same run; a parameter or the data changes it
    moved = tmp_path / "elsewhere.bse"
    shutil.copyfile(FIXTURE, moved)
    assert plan_runs(experiment(tmp_path, dataset=str(moved)))[0][1] == run_id
    other = {**PINNED["strategy"], "params": {**PINNED["strategy"]["params"], "refresh_ms": 101}}
    assert plan_runs(experiment(tmp_path, strategy=other))[0][1] != run_id
    with open(moved, "r+b") as f:
        f.seek(-1, 2)
        last = f.read(1)
        f.seek(-1, 2)
        f.write(bytes([last[0] ^ 1]))
    assert plan_runs(experiment(tmp_path, dataset=str(moved)))[0][1] != run_id

    # a Python strategy: its source is part of the identity
    py = plan_runs(experiment(tmp_path, strategy={"python": "research.strategies.passive_quoter:PassiveQuoter",
                                                  "params": PINNED["strategy"]["params"]}))[0][2]
    assert set(py["python_strategy"]["sources"]) == {"passive_quoter.py", "base.py"}
    cfg = bs.SimConfig({**spec.config, "output": {**spec.config["output"], "dir": "x"}})
    assert run_id_of(identity(cfg, spec.strategy)) == run_id


# ---- P5.3 runner ----

def test_run_then_cache_hit_with_identical_numbers(tmp_path):
    exp = experiment(tmp_path)
    first = run_experiment(exp, workers=1, progress=None)
    (rec,) = first.runs
    assert rec.status == "ran" and rec.output_hash == PINNED_HASH  # the C++ test's pinned result
    stored = first.load(rec)
    assert stored.meta["identity"]["format"] == "bs.run_identity/1" and stored.meta["experiment"]["seed"] == 7
    assert stored.meta["run_id"] == rec.run_id and stored.meta["git"] is None or "sha" in stored.meta["git"]
    reports = stored.frame("exec_reports")
    assert reports.height == stored.meta["outputs"]["exec_reports.csv"]["rows"]
    assert reports.schema["price"] == pl.Decimal(38, 8)
    assert sorted(p.name for p in stored.path.iterdir()) == sorted(
        [f"{t}.parquet" for t in ("orders", "exec_reports", "request_rejects", "bbo_samples", "positions")]
        + ["run_meta.json"])

    again = run_experiment(exp, workers=1, progress=None)
    assert [r.status for r in again.runs] == ["cached"]
    assert (again.runs[0].output_hash, again.runs[0].pnl, again.runs[0].fills) == (rec.output_hash, rec.pnl, rec.fills)

    forced = run_experiment(exp, workers=1, force=True, progress=None)
    assert forced.runs[0].status == "ran" and forced.runs[0].output_hash == PINNED_HASH

    summary = json.loads((tmp_path / "results" / "experiments" / "pinned" / "summary.json").read_text())
    assert summary["counts"] == {"ran": 1} and summary["runs"][0]["run_id"] == rec.run_id
    assert not list((tmp_path / "results").glob(".tmp-*"))  # no scratch directories left


def test_parallel_runs_python_strategies_and_isolates_failures(tmp_path):
    exp = experiment(tmp_path, name="isolation", seeds=[1, 2],
                     strategy={"python": "test_experiments:Boom", "params": {}},
                     sweep={"params": {"strategy.params.fail": [False, True]}})
    lines = []
    result = run_experiment(exp, workers=2, progress=lines.append)
    status = {(r.params["strategy.params.fail"], r.seed): r.status for r in result.runs}
    assert status == {(False, 1): "ran", (False, 2): "ran", (True, 1): "failed", (True, 2): "failed"}
    assert all("RuntimeError: strategy bug" in r.error for r in result.failed)
    assert sum("FAILED" in line for line in lines) == 2 and "2 failed, 2 ran" in lines[-1]
    df = result.to_polars()
    assert df.height == 4 and "param.strategy.params.fail" in df.columns
    # Failed runs aren't stored, so they run again next time; the good ones are cached
    again = run_experiment(exp, workers=2, progress=None)
    assert sorted(r.status for r in again.runs) == ["cached", "cached", "failed", "failed"]


def test_a_worker_crash_fails_only_its_run(tmp_path):
    exp = experiment(tmp_path, name="crash", seeds=[1, 2, 3],
                     strategy={"python": "test_experiments:Crash", "params": {}},
                     sweep={"params": {"strategy.params.fail": [False, True]}})
    result = run_experiment(exp, workers=2, progress=None)
    status = {(r.params["strategy.params.fail"], r.seed): r.status for r in result.runs}
    assert [status[(False, s)] for s in (1, 2, 3)] == ["ran"] * 3
    assert [status[(True, s)] for s in (1, 2, 3)] == ["failed"] * 3
    assert all("worker process died" in r.error for r in result.failed)


def test_command_line(tmp_path, capsys):
    path = write_yaml(tmp_path, {**PINNED, "results": str(tmp_path / "results")})
    assert run_main([str(path), "--dry-run"]) == 0
    assert "1 runs, 0 cached" in capsys.readouterr().out
    assert run_main([str(path), "--workers", "1"]) == 0
    assert run_main([str(path), "--dry-run"]) == 0
    assert "1 runs, 1 cached" in capsys.readouterr().out
    bad = write_yaml(tmp_path, {**PINNED, "queue_model": {"model": "nope"}})
    assert run_main([str(bad)]) == 2
    assert "queue_model" in capsys.readouterr().err


# ---- suites and labels (P5.12) ----

LATENCY_5MS = {leg: {"model": "constant", "us": 5000} for leg in ("feed", "entry", "ack")}
LATENCY_0 = {leg: {"model": "constant", "us": 0} for leg in ("feed", "entry", "ack")}


def suite_doc(tmp_path):
    return {
        "description": "two strategies",
        "defaults": {"dataset": str(FIXTURE), "results": str(tmp_path / "results"), "seeds": [3],
                     "strategy": {"name": "passive_quoter"},
                     "sweep": {"params": {"latency": [LATENCY_0, LATENCY_5MS]}, "labels": {"latency": ["0 ms", "5 ms"]}}},
        "experiments": [{"name": "quoter"},
                        {"name": "twap", "strategy": {"name": "twap_taker",
                                                      "params": {"side": "sell", "quantity": "0.01", "slices": 2,
                                                                 "duration_ms": 20000}}}],
    }


def test_a_suite_loads_with_defaults_and_labels(tmp_path):
    exps = Experiment.load_all(write_yaml(tmp_path, suite_doc(tmp_path)))
    assert [e.name for e in exps] == ["quoter", "twap"]
    assert exps[0].strategy.name == "passive_quoter" and exps[1].strategy.name == "twap_taker"
    assert all(e.seeds == [3] and len(e.plan()) == 2 for e in exps)
    assert exps[0].label("latency", LATENCY_5MS) == "5 ms" and exps[0].label("seed", 3) == "3"
    with pytest.raises(ValueError, match="load_all"):
        Experiment.load(write_yaml(tmp_path, suite_doc(tmp_path)))


@pytest.mark.parametrize("change, message", [
    (lambda d: d["experiments"].append({"name": "quoter"}), "names must differ"),
    (lambda d: d.update(extra=1), "not extra"),
    (lambda d: d.update(experiments=[]), "non-empty"),
    (lambda d: d["defaults"]["sweep"]["labels"].update(latency=["0 ms"]), "one distinct label per value"),
    (lambda d: d["defaults"]["sweep"]["labels"].update(latency=["x", "x"]), "one distinct label per value"),
    (lambda d: d["defaults"]["sweep"]["labels"].update(seed=["a"]), "label a key of sweep.params"),
    (lambda d: d["experiments"][1]["strategy"]["params"].update(slices=0), r"experiments\[1\] \(twap\)"),
])
def test_suite_errors(tmp_path, change, message):
    doc = suite_doc(tmp_path)
    change(doc)
    with pytest.raises(ValueError, match=message):
        Experiment.load_all(write_yaml(tmp_path, doc))


def test_a_suite_runs_from_the_command_line_with_labels(tmp_path, capsys):
    path = write_yaml(tmp_path, suite_doc(tmp_path))
    assert run_main([str(path), "--dry-run"]) == 0
    out = capsys.readouterr().out
    assert "latency=5 ms seed=3" in out and "quoter: 2 runs" in out and "twap: 2 runs" in out
    assert run_main([str(path), "--workers", "1"]) == 0
    root = tmp_path / "results" / "experiments"
    for name in ("quoter", "twap"):
        summary = json.loads((root / name / "summary.json").read_text())
        assert [r["labels"] for r in summary["runs"]] == [{"latency": "0 ms"}, {"latency": "5 ms"}]
        page = (root / name / "report.html").read_text(encoding="utf-8")
        assert ">5 ms<" in page and "&quot;us&quot;" not in page  # the label, not the latency object


def test_the_shipped_sensitivity_suite_is_valid(tmp_path):
    """configs/sensitivity.yaml reads the packed Nov 10 data, which isn't in git: check it on the slice"""
    doc = yaml.safe_load((ROOT / "research" / "experiments" / "configs" / "sensitivity.yaml").read_text())
    doc["defaults"].update(dataset=str(FIXTURE), results=str(tmp_path / "results"))
    doc["defaults"].pop("window")
    exps = Experiment.load_all(write_yaml(tmp_path, doc))
    assert [e.name for e in exps] == ["sensitivity_mm", "sensitivity_twap", "sensitivity_imbalance"]
    assert all(len(e.plan()) == 16 for e in exps)
    labels = {(e.label("latency", s.params["latency"]), e.label("queue_model", s.params["queue_model"]))
              for e in exps[:1] for s in e.plan()}
    assert len(labels) == 16 and ("50 ms", "power 2") in labels


def test_the_shipped_impact_sensitivity_suite_is_valid(tmp_path):
    """configs/sensitivity_impact.yaml (P6.8): tau x latency, four experiments of 16 runs, each impact
    setting reaching the run config"""
    doc = yaml.safe_load((ROOT / "research" / "experiments" / "configs" / "sensitivity_impact.yaml").read_text())
    doc["defaults"].update(dataset=str(FIXTURE), results=str(tmp_path / "results"))
    doc["defaults"].pop("window")
    exps = Experiment.load_all(write_yaml(tmp_path, doc))
    assert [e.name for e in exps] == ["sensitivity_impact_mm", "sensitivity_impact_twap", "sensitivity_impact_imbalance",
                                      "sensitivity_impact_twap_large"]
    assert all(len(e.plan()) == 16 for e in exps)
    taus = {s.config["impact"]["half_life_ms"] for s in exps[0].plan()}
    assert taus == {0, 1000, 10000, 60000}
    assert {exps[0].label("impact.half_life_ms", s.params["impact.half_life_ms"]) for s in exps[0].plan()} == {
        "tau 0", "tau 1 s", "tau 10 s", "tau 60 s"}
