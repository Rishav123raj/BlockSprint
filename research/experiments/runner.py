"""Running an experiment (P5.3): every run of its plan, in parallel processes, through the store.

    python -m research.run research/experiments/configs/quoter_fixture.yaml [--workers N] [--force] [--dry-run]

- Runs already in the store are cache hits and aren't run again (--force runs them anyway).
- The rest run in a ProcessPoolExecutor, one Simulator per process at a time. A run that raises is
  recorded as failed with its error and the others carry on. A worker process that dies (a crash in native
  code) breaks the pool: every unfinished run is then retried alone in its own process, so only the one that
  crashes fails.
- Progress: one line per finished run. At the end, results/experiments/<name>/summary.json lists every run
  (point, seed, sweep values, run_id, status, time, output hash, PnL, fills, error) and is returned as an
  ExperimentResult. The exit code of research.run is 1 if any run failed.
"""

from __future__ import annotations

import json
import os
import shutil
import time
import traceback
from concurrent.futures import ProcessPoolExecutor, as_completed
from concurrent.futures.process import BrokenProcessPool
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable

import blocksprint_sim as bs
from research.experiments.schema import Experiment, RunSpec, make_python_strategy
from research.experiments.store import Store, git_info, identity, run_id_of
from research.sim.results import Results


@dataclass
class RunRecord:
    """One run of an experiment, as the summary lists it."""
    point: int
    seed: int
    params: dict[str, Any]
    run_id: str
    status: str = "pending"        # ran | cached | failed
    wall_s: float | None = None
    output_hash: str | None = None
    pnl: str | None = None         # exchange account PnL at the final mark (exact decimal)
    fills: int | None = None
    error: str | None = None


@dataclass
class ExperimentResult:
    name: str
    store: Store
    runs: list[RunRecord] = field(default_factory=list)
    wall_s: float = 0.0

    @property
    def failed(self) -> list[RunRecord]:
        return [r for r in self.runs if r.status == "failed"]

    def counts(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for r in self.runs:
            out[r.status] = out.get(r.status, 0) + 1
        return out

    def load(self, record: RunRecord):
        """The stored run of a record (research.experiments.store.StoredRun)."""
        return self.store.load(record.run_id)

    def to_polars(self):
        import polars as pl

        rows = []
        for r in self.runs:
            row = asdict(r)
            params = row.pop("params")
            rows.append({**{f"param.{k}": (v if isinstance(v, (int, float, str)) else json.dumps(v))
                            for k, v in params.items()}, **row})
        return pl.DataFrame(rows)


def plan_runs(exp: Experiment) -> list[tuple[RunSpec, str, dict[str, Any]]]:
    """Each run with its run_id and identity."""
    out = []
    for spec in exp.plan():
        cfg = bs.SimConfig({**spec.config, "output": {**spec.config["output"], "dir": "unused"}})
        ident = identity(cfg, spec.strategy)
        out.append((spec, run_id_of(ident), ident))
    return out


def execute(spec: RunSpec, run_id: str, ident: dict[str, Any], store_root: str) -> dict[str, Any]:
    """Runs one spec into the store. In a worker process (or in this one). Never raises: errors are returned."""
    t0 = time.perf_counter()
    try:
        store = Store(store_root)
        scratch = store.scratch(run_id)
        cfg = bs.SimConfig({**spec.config, "output": {**spec.config["output"], "dir": str(scratch)}})
        strategy = make_python_strategy(spec.strategy) if "python" in spec.strategy else None
        sim = bs.Simulator(cfg, strategy, spec.strategy.get("callback_mode"))
        sim.run()
        extra = {"identity": ident, "experiment": {"name": spec.experiment, "point": spec.point, "seed": spec.seed,
                                                   "params": spec.params}, "git": git_info()}
        stored = store.save(run_id, scratch, Results.from_simulator(sim), extra)
        return {"ok": True, "wall_s": time.perf_counter() - t0, "meta": _summary(stored.meta)}
    except Exception as e:  # failure isolation: report it, don't take the experiment down
        return {"ok": False, "wall_s": time.perf_counter() - t0,
                "error": f"{type(e).__name__}: {e}\n{traceback.format_exc(limit=8)}"}


def _summary(meta: dict[str, Any]) -> dict[str, Any]:
    account = meta["sessions"][0]["exchange_account"]
    return {"output_hash": meta["output_hash"], "pnl": account["pnl"], "fills": account["fills"]}


def run_experiment(exp: Experiment, workers: int | None = None, force: bool = False,
                   progress: Callable[[str], None] | None = print) -> ExperimentResult:
    """Runs every run of exp that isn't in the store (all of them with force). workers: processes (default:
    CPUs, at most the number of runs); 1 runs in this process."""
    t0 = time.perf_counter()
    store = Store(exp.results)
    planned = plan_runs(exp)
    result = ExperimentResult(exp.name, store)
    todo: list[tuple[RunRecord, RunSpec, dict[str, Any]]] = []
    seen: dict[str, RunRecord] = {}
    for spec, run_id, ident in planned:
        rec = RunRecord(spec.point, spec.seed, spec.params, run_id)
        result.runs.append(rec)
        if run_id in seen:  # the same run twice in one plan (e.g. a swept value that changes nothing)
            rec.status = "duplicate"
            continue
        seen[run_id] = rec
        if store.has(run_id) and not force:
            rec.status = "cached"
            m = _summary(store.load(run_id).meta)
            rec.output_hash, rec.pnl, rec.fills = m["output_hash"], m["pnl"], m["fills"]
        else:
            todo.append((rec, spec, ident))

    total = len(result.runs)
    done = [sum(r.status != "pending" for r in result.runs)]
    say = progress or (lambda _: None)
    for rec in result.runs:
        if rec.status == "cached":
            say(_line(done_count=None, total=total, rec=rec, spec_label=_label(rec)))

    def finish(rec: RunRecord, out: dict[str, Any]) -> None:
        rec.wall_s = round(out["wall_s"], 3)
        if out["ok"]:
            rec.status = "ran"
            rec.output_hash, rec.pnl, rec.fills = out["meta"]["output_hash"], out["meta"]["pnl"], out["meta"]["fills"]
        else:
            rec.status, rec.error = "failed", out["error"]
        done[0] += 1
        say(_line(done[0], total, rec, _label(rec)))

    if force:
        for rec, _, _ in todo:
            if store.has(rec.run_id):
                shutil.rmtree(store.path(rec.run_id))

    n_workers = max(1, min(workers or os.cpu_count() or 1, len(todo) or 1))
    if n_workers == 1:
        for rec, spec, ident in todo:
            finish(rec, execute(spec, rec.run_id, ident, str(store.root)))
    else:
        _run_pool(todo, n_workers, str(store.root), finish)

    for rec in result.runs:
        if rec.status == "duplicate":
            first = seen[rec.run_id]
            rec.output_hash, rec.pnl, rec.fills, rec.error = first.output_hash, first.pnl, first.fills, first.error
    result.wall_s = round(time.perf_counter() - t0, 3)
    _write_summary(exp, result)
    counts = result.counts()
    say(f"{exp.name}: {total} runs in {result.wall_s:.1f} s ("
        + ", ".join(f"{n} {k}" for k, n in sorted(counts.items())) + f"); summary {_summary_path(exp)}")
    return result


def _run_pool(todo, n_workers: int, store_root: str, finish) -> None:
    retry = []
    with ProcessPoolExecutor(max_workers=n_workers) as pool:
        futures = {pool.submit(execute, spec, rec.run_id, ident, store_root): (rec, spec, ident)
                   for rec, spec, ident in todo}
        for fut in as_completed(futures):
            try:
                out = fut.result()
            except BrokenProcessPool:  # a worker died: every unfinished run lands here
                retry.append(futures[fut])
                continue
            finish(futures[fut][0], out)
    # Each again on its own: the run that crashed fails alone, the others complete
    for rec, spec, ident in retry:
        with ProcessPoolExecutor(max_workers=1) as pool:
            try:
                out = pool.submit(execute, spec, rec.run_id, ident, store_root).result()
            except BrokenProcessPool:
                out = {"ok": False, "wall_s": 0.0, "error": "BrokenProcessPool: the worker process died (native crash?)"}
        finish(rec, out)


def _label(rec: RunRecord) -> str:
    parts = [f"{k}={v}" for k, v in rec.params.items()]
    return " ".join(parts + [f"seed={rec.seed}"])


def _line(done_count: int | None, total: int, rec: RunRecord, spec_label: str) -> str:
    width = len(str(total))
    pos = f"[{done_count:>{width}}/{total}]" if done_count is not None else f"[{'-' * width}/{total}]"
    head = f"{pos} {rec.status:<6} {rec.run_id}  {spec_label}"
    if rec.status == "failed":
        return f"{head}  FAILED {(rec.error or '').splitlines()[0]}"
    t = f"{rec.wall_s:.1f} s  " if rec.wall_s is not None else ""
    return f"{head}  {t}pnl {rec.pnl} fills {rec.fills}"


def _summary_path(exp: Experiment) -> Path:
    return Path(exp.results) / "experiments" / exp.name / "summary.json"


def _write_summary(exp: Experiment, result: ExperimentResult) -> None:
    path = _summary_path(exp)
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = {"format": "bs.experiment_summary/1", "experiment": exp.model_dump(mode="json", by_alias=True),
           "git": git_info(), "wall_s": result.wall_s, "counts": result.counts(),
           "runs": [asdict(r) for r in result.runs]}
    tmp = path.with_suffix(".json.tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)
