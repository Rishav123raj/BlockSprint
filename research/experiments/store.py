"""The run store (P5.2): one directory per run, named by what determines its results.

    results/<run_id>/
        run_meta.json            the simulator's run_meta.json, plus run_id, identity, point and provenance
        orders.parquet           the five output tables, as Results.to_polars() gives them (exact decimals,
        exec_reports.parquet     UTC datetimes); equal to the simulator's CSVs
        request_rejects.parquet
        bbo_samples.parquet
        positions.parquet

run_id is the SHA-256 (first 16 hex digits) of the run's identity:
  - the resolved run config without output.dir and without the dataset path (the data is identified by
    its content below, so the same data under another path is the same run);
  - the data: the .bse file's SHA-256 (from the dataset manifest, else hashed here);
  - the engine: the SHA-256 of the compiled blocksprint_sim module, so a rebuilt engine is a new run;
  - a Python strategy: its class path, parameters, callback mode and the SHA-256 of its module's source
    and of research/strategies/base.py.
The plan named the git SHA for the engine; the module and source hashes also catch uncommitted changes. The
git commit is still recorded (provenance), not part of the identity.

A run that exists is a cache hit. Runs are written to a temporary directory and renamed into place when
complete, so a directory under its run_id is always a whole run, even if a process dies or two write it.
"""

from __future__ import annotations

import functools
import hashlib
import inspect
import json
import os
import shutil
import subprocess
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import blocksprint_sim as bs
from research.sim.results import TABLES, Results

IDENTITY_FORMAT = "bs.run_identity/1"
ROOT = Path(__file__).resolve().parents[2]


def sha256_file(path: str | Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


@functools.lru_cache(maxsize=None)
def engine_sha256() -> str:
    """The compiled blocksprint_sim module's SHA-256."""
    return sha256_file(bs.__file__)


@functools.lru_cache(maxsize=None)
def _data_sha256(path: str, size: int, mtime_ns: int) -> str:
    return sha256_file(path)


def data_sha256(cfg: bs.SimConfig) -> str:
    if cfg.data_sha256:
        return cfg.data_sha256
    st = os.stat(cfg.data_file)
    return _data_sha256(os.path.abspath(cfg.data_file), st.st_size, st.st_mtime_ns)


def strategy_identity(strategy: dict[str, Any]) -> dict[str, Any] | None:
    """For a Python strategy: what of its code and settings the results depend on. None for a C++ one (its
    name and params are in the config, its code in the engine)."""
    if "python" not in strategy:
        return None
    from research.experiments.schema import load_class

    cls = load_class(strategy["python"])
    sources = {}
    for path in {inspect.getsourcefile(cls), str(ROOT / "research" / "strategies" / "base.py")}:
        if path and os.path.exists(path):
            sources[Path(path).name] = sha256_file(path)
    return {"python": strategy["python"], "params": strategy.get("params", {}),
            "callback_mode": strategy.get("callback_mode"), "sources": dict(sorted(sources.items()))}


def identity(cfg: bs.SimConfig, strategy: dict[str, Any]) -> dict[str, Any]:
    resolved = cfg.resolved
    resolved.pop("dataset", None)
    resolved.get("output", {}).pop("dir", None)
    return {"format": IDENTITY_FORMAT, "config": resolved, "data_sha256": data_sha256(cfg),
            "engine_sha256": engine_sha256(), "python_strategy": strategy_identity(strategy)}


def run_id_of(ident: dict[str, Any]) -> str:
    canonical = json.dumps(ident, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(canonical.encode()).hexdigest()[:16]


@functools.lru_cache(maxsize=None)
def git_info() -> dict[str, Any] | None:
    """The source tree's commit and whether tracked files changed; None outside a git checkout."""
    try:
        sha = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True,
                             check=True, timeout=10).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, check=True, timeout=10).stdout.strip() != ""
        return {"sha": sha, "dirty": dirty}
    except (OSError, subprocess.SubprocessError):
        return None


@dataclass
class StoredRun:
    """A run in the store."""
    run_id: str
    path: Path
    meta: dict[str, Any]

    @property
    def output_hash(self) -> str:
        return self.meta["output_hash"]

    def frame(self, table: str):
        """One output table as a polars DataFrame."""
        import polars as pl

        if table not in TABLES:
            raise ValueError(f"unknown table {table!r}: one of {', '.join(TABLES)}")
        return pl.read_parquet(self.path / f"{table}.parquet")

    def frames(self) -> dict[str, Any]:
        return {t: self.frame(t) for t in TABLES}


class Store:
    def __init__(self, root: str | Path = "results"):
        self.root = Path(root)

    def path(self, run_id: str) -> Path:
        return self.root / run_id

    def has(self, run_id: str) -> bool:
        return (self.path(run_id) / "run_meta.json").is_file()

    def load(self, run_id: str) -> StoredRun:
        p = self.path(run_id)
        with open(p / "run_meta.json", encoding="utf-8") as f:
            return StoredRun(run_id, p, json.load(f))

    def scratch(self, run_id: str) -> Path:
        """A fresh temporary directory for writing run_id (the simulator writes its CSVs there)."""
        p = self.root / f".tmp-{run_id}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
        p.mkdir(parents=True)
        return p

    def save(self, run_id: str, scratch: Path, results: Results, extra: dict[str, Any]) -> StoredRun:
        """Writes the tables and run_meta.json into scratch, then moves it to run_id. If another process
        finished the same run meanwhile, keeps theirs (identical by construction)."""
        try:
            for table, df in results.to_polars().items():
                df.write_parquet(scratch / f"{table}.parquet")
            meta = {**results.meta, **extra, "run_id": run_id}
            for csv in list(scratch.glob("*.csv")) + [scratch / "output_hash", scratch / "run_meta.json"]:
                csv.unlink(missing_ok=True)
            with open(scratch / "run_meta.json", "w", encoding="utf-8", newline="\n") as f:
                json.dump(meta, f, indent=2)
                f.write("\n")
            target = self.path(run_id)
            try:
                os.replace(scratch, target)
            except OSError:
                if not self.has(run_id):
                    raise
                shutil.rmtree(scratch, ignore_errors=True)
            return self.load(run_id)
        except BaseException:
            shutil.rmtree(scratch, ignore_errors=True)
            raise
