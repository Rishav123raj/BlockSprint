"""Experiment files (P5.1): one YAML file describes a set of simulator runs.

    name: quoter_latency                      # names the experiment's summary (results/experiments/<name>/)
    dataset: data/packed/binance_spot_btc_eth_1105_1107
    symbol: BTCUSDT
    window: {from: "2026-11-05T02:00:00Z", to: "2026-11-05T03:00:00Z", warmup_s: 60}
    seeds: [1, 2, 3]                          # every point runs once per seed
    latency_preset: binance_spot_tokyo_vps
    queue_model: {model: cancel_from_back}
    strategy:
      name: passive_quoter                    # a C++ strategy (bs.strategy_names()), or
      # python: research.strategies.passive_quoter:PassiveQuoter   (a class; params are its keyword arguments)
      params: {size: "0.001", refresh_ms: 500}
    sweep:
      mode: grid                              # grid: every combination; random: `samples` draws
      params:
        latency_preset: [zero, binance_spot_tokyo_vps, binance_spot_home_pc]
        queue_model.model: [cancel_from_back, cancel_from_front, pro_rata]
        strategy.params.refresh_ms: [100, 500]

The simulator settings (dataset ... limits) are the run-config format (docs/simulator_architecture.md section
13.1); the C++ side validates them, so a bad value is reported with its key when the file is loaded, before
anything runs. Sweep keys are dotted paths into that config, as `bs_sim run --set` takes them;
`strategy.params.X` sets a strategy parameter, for C++ and Python strategies alike. A value replaces what is
at its key: to sweep queue models that take parameters, sweep whole objects, e.g. queue_model:
[{model: pro_rata}, {model: prob_power, n: 2}] (sweeping queue_model.model would keep the base's n). In random
mode a value
is a list to choose from or a distribution: {uniform: [a, b]}, {log_uniform: [a, b]}, {int: [a, b]}
(inclusive), drawn with the sweep's own seed.

Relative paths (dataset, results) are relative to the working directory, as for bs_sim.
"""

from __future__ import annotations

import copy
import importlib
import itertools
import json
import math
import random
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal

import yaml
from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

import blocksprint_sim as bs

CallbackMode = Literal["every_event", "on_change", "timer_only"]


class Window(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True)
    from_: str | int | None = Field(default=None, alias="from")
    to: str | int | None = None
    warmup_s: float | None = None

    def as_config(self) -> dict[str, Any]:
        out: dict[str, Any] = {}
        if self.from_ is not None:
            out["from"] = self.from_
        if self.to is not None:
            out["to"] = self.to
        if self.warmup_s is not None:
            out["warmup_s"] = self.warmup_s
        return out


class StrategySpec(BaseModel):
    """A C++ strategy by registry name, or a Python strategy class as "package.module:Class"."""
    model_config = ConfigDict(extra="forbid")
    name: str | None = None
    python: str | None = None
    params: dict[str, Any] = Field(default_factory=dict)
    callback_mode: CallbackMode | None = None  # Python only; default: the class's own

    @model_validator(mode="after")
    def _one_kind(self) -> StrategySpec:
        if (self.name is None) == (self.python is None):
            raise ValueError("give exactly one of strategy.name (C++) and strategy.python (module:Class)")
        if self.name is not None:
            if self.name not in bs.strategy_names():
                raise ValueError(f"unknown C++ strategy {self.name!r}: one of {', '.join(bs.strategy_names())}")
            if self.callback_mode is not None:
                raise ValueError("strategy.callback_mode applies to Python strategies only")
        else:
            load_class(self.python)  # fails now, not in a worker
        return self


class Distribution(BaseModel):
    """A random-sweep distribution: exactly one of uniform, log_uniform, int (each [low, high])."""
    model_config = ConfigDict(extra="forbid")
    uniform: tuple[float, float] | None = None
    log_uniform: tuple[float, float] | None = None
    int_: tuple[int, int] | None = Field(default=None, alias="int")

    @model_validator(mode="after")
    def _one(self) -> Distribution:
        given = [k for k in ("uniform", "log_uniform", "int_") if getattr(self, k) is not None]
        if len(given) != 1:
            raise ValueError("a distribution is exactly one of {uniform: [a, b]}, {log_uniform: [a, b]}, {int: [a, b]}")
        low, high = getattr(self, given[0])
        if low > high:
            raise ValueError(f"distribution: low {low} > high {high}")
        if self.log_uniform is not None and low <= 0:
            raise ValueError("log_uniform needs low > 0")
        return self

    def draw(self, rng: random.Random) -> float | int:
        if self.uniform is not None:
            return rng.uniform(*self.uniform)
        if self.log_uniform is not None:
            low, high = self.log_uniform
            return math.exp(rng.uniform(math.log(low), math.log(high)))
        assert self.int_ is not None
        return rng.randint(*self.int_)


class Sweep(BaseModel):
    model_config = ConfigDict(extra="forbid")
    mode: Literal["grid", "random"] = "grid"
    params: dict[str, list[Any] | Distribution]
    samples: int | None = Field(default=None, ge=1)  # random mode
    seed: int = 0                                    # random mode: the draws' seed
    labels: dict[str, list[str]] = Field(default_factory=dict)  # names for a key's values, in the same order

    @model_validator(mode="after")
    def _labels(self) -> Sweep:
        for key, names in self.labels.items():
            values = self.params.get(key)
            if not isinstance(values, list):
                raise ValueError(f"sweep.labels.{key}: label a key of sweep.params that has a list of values")
            if len(names) != len(values) or len(set(names)) != len(names):
                raise ValueError(f"sweep.labels.{key}: one distinct label per value ({len(values)})")
        return self

    def label(self, key: str, value: Any) -> str:
        """The name of a swept value: its label if the sweep gives one, else the value itself, shortened."""
        values = self.params.get(key)
        if key in self.labels and isinstance(values, list):
            for v, name in zip(values, self.labels[key]):
                if v == value:
                    return name
        return _short(value)

    @field_validator("params")
    @classmethod
    def _non_empty(cls, v: dict[str, Any]) -> dict[str, Any]:
        if not v:
            raise ValueError("sweep.params: give at least one key")
        for key, values in v.items():
            if isinstance(values, list) and not values:
                raise ValueError(f"sweep.params.{key}: empty list")
            if key in ("dataset", "output", "strategy") or key.startswith(("output.", "strategy.name", "strategy.owner")):
                raise ValueError(f"sweep.params.{key}: can't be swept (sweep strategy.params.*, or use two experiments)")
        return v

    @model_validator(mode="after")
    def _mode(self) -> Sweep:
        if self.mode == "grid":
            if self.samples is not None:
                raise ValueError("sweep.samples is for random mode")
            if any(isinstance(v, Distribution) for v in self.params.values()):
                raise ValueError("distributions need sweep.mode: random (grid takes lists)")
        elif self.samples is None:
            raise ValueError("sweep.mode random needs sweep.samples")
        return self

    def points(self) -> list[dict[str, Any]]:
        """Each point as {key: value}, in a fixed order."""
        keys = list(self.params)
        if self.mode == "grid":
            lists = [self.params[k] for k in keys]
            return [dict(zip(keys, combo)) for combo in itertools.product(*lists)]
        rng = random.Random(self.seed)
        out = []
        for _ in range(self.samples or 0):
            point = {}
            for k in keys:
                v = self.params[k]
                point[k] = v.draw(rng) if isinstance(v, Distribution) else rng.choice(v)
            out.append(point)
        return out


class Experiment(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(pattern=r"^[A-Za-z0-9_.-]+$")
    description: str = ""
    dataset: str
    symbol: str | None = None
    window: Window | None = None
    seeds: list[int] = Field(default_factory=lambda: [0], min_length=1)
    tie_break: Literal["data_first", "orders_first"] | None = None
    latency_preset: str | None = None
    latency: dict[str, Any] | None = None
    latency_regime: dict[str, Any] | None = None
    queue_model: dict[str, Any] | None = None
    impact: dict[str, Any] | None = None
    fees: dict[str, Any] | None = None
    limits: dict[str, Any] | None = None
    risk: dict[str, Any] | None = None
    sample_ms: int = Field(default=1000, ge=0)
    strategy: StrategySpec
    sweep: Sweep | None = None
    results: str = "results"  # the store's root

    @field_validator("seeds")
    @classmethod
    def _unique(cls, v: list[int]) -> list[int]:
        if len(set(v)) != len(v):
            raise ValueError("seeds: each seed once")
        return v

    @classmethod
    def load(cls, path: str | Path) -> Experiment:
        """Reads and validates a YAML file of one experiment, and checks every run it describes (plan())."""
        exps = cls.load_all(path)
        if len(exps) != 1:
            raise ValueError(f"{path}: a suite of {len(exps)} experiments: use Experiment.load_all")
        return exps[0]

    @classmethod
    def load_all(cls, path: str | Path) -> list[Experiment]:
        """The experiments of a YAML file: one experiment, or a suite

            defaults: {dataset: ..., sweep: ...}      # shared settings
            experiments:                              # each overrides defaults key by key
              - {name: a, strategy: ...}
              - {name: b, strategy: ...}

        Each is validated and its runs checked (plan()). Names must differ."""
        with open(path, encoding="utf-8") as f:
            data = yaml.safe_load(f)
        if not isinstance(data, dict):
            raise ValueError(f"{path}: expected a mapping at the top level")
        if "experiments" not in data:
            items = [data]
        else:
            unknown = set(data) - {"experiments", "defaults", "description"}
            if unknown:
                raise ValueError(f"{path}: a suite has experiments, defaults and description, not {', '.join(sorted(unknown))}")
            defaults = data.get("defaults") or {}
            if not isinstance(defaults, dict) or not isinstance(data["experiments"], list) or not data["experiments"]:
                raise ValueError(f"{path}: defaults must be a mapping and experiments a non-empty list")
            items = []
            for i, item in enumerate(data["experiments"]):
                if not isinstance(item, dict):
                    raise ValueError(f"{path}: experiments[{i}] must be a mapping")
                items.append({**copy.deepcopy(defaults), **item})
        exps = []
        for i, item in enumerate(items):
            try:
                exp = cls.model_validate(item)
                exp.plan()
            except ValueError as e:
                where = f"experiments[{i}] ({item.get('name', '?')})" if len(items) > 1 or "experiments" in data else ""
                raise ValueError(f"{where}: {e}" if where else str(e)) from None
            exps.append(exp)
        names = [e.name for e in exps]
        if len(set(names)) != len(names):
            raise ValueError(f"{path}: experiment names must differ ({', '.join(names)})")
        return exps

    def label(self, key: str, value: Any) -> str:
        """The name of a swept value (Sweep.label)."""
        return self.sweep.label(key, value) if self.sweep else _short(value)

    def base_config(self) -> dict[str, Any]:
        """The run config before the sweep and the seed (output.dir is set per run by the runner)."""
        cfg: dict[str, Any] = {"dataset": self.dataset}
        for key in ("symbol", "tie_break", "latency_preset", "latency", "latency_regime", "queue_model", "impact", "fees",
                    "limits", "risk"):
            value = getattr(self, key)
            if value is not None:
                cfg[key] = copy.deepcopy(value)
        if self.window is not None:
            cfg["window"] = self.window.as_config()
        cfg["output"] = {"sample_ms": self.sample_ms}
        return cfg

    def plan(self) -> list[RunSpec]:
        """Every run: the sweep's points (or one) times the seeds, each checked by the C++ config resolver."""
        points = self.sweep.points() if self.sweep else [{}]
        specs = []
        for i, point in enumerate(points):
            for seed in self.seeds:
                spec = RunSpec.build(self, i, point, seed)
                spec.check()
                specs.append(spec)
        return specs


@dataclass
class RunSpec:
    """One run of an experiment: its config, its strategy and where it comes from."""
    experiment: str
    point: int                     # index of the sweep point
    seed: int
    params: dict[str, Any]         # the sweep values of this point
    config: dict[str, Any]         # the run config, without output.dir
    strategy: dict[str, Any] = field(default_factory=dict)  # StrategySpec as a dict, with this point's params

    @classmethod
    def build(cls, exp: Experiment, point: int, params: dict[str, Any], seed: int) -> RunSpec:
        cfg = exp.base_config()
        strategy = exp.strategy.model_dump(exclude_none=True)
        strategy["params"] = copy.deepcopy(strategy.get("params", {}))
        for key, value in params.items():
            if key.startswith("strategy.params."):
                strategy["params"][key[len("strategy.params."):]] = value
            else:
                _set_path(cfg, key, value)
        cfg["seed"] = seed
        if "name" in strategy:
            cfg["strategy"] = {"name": strategy["name"], "params": strategy["params"]}
        else:
            cfg["strategy"] = {"name": "null"}  # the C++ side runs the Python object instead
        return cls(exp.name, point, seed, dict(params), cfg, strategy)

    def check(self) -> None:
        """The config resolves (C++), and a Python strategy can be built with these params."""
        try:
            bs.SimConfig({**self.config, "output": {**self.config["output"], "dir": "unused"}})
        except ValueError as e:
            raise ValueError(f"point {self.point} {self.params}, seed {self.seed}: {e}") from None
        if "python" in self.strategy:
            try:
                make_python_strategy(self.strategy)
            except Exception as e:
                raise ValueError(f"point {self.point} {self.params}: strategy {self.strategy['python']}: {e}") from None

    def label(self) -> str:
        parts = [f"{k}={_short(v)}" for k, v in self.params.items()]
        return " ".join(parts + [f"seed={self.seed}"])


def load_class(path: str | None) -> type:
    """"package.module:Class" -> the class."""
    if not path or ":" not in path:
        raise ValueError(f"strategy.python: expected 'package.module:Class', got {path!r}")
    module_name, _, attr = path.partition(":")
    module = importlib.import_module(module_name)
    obj = getattr(module, attr, None)
    if not isinstance(obj, type):
        raise ValueError(f"strategy.python: {path} is not a class")
    return obj


def make_python_strategy(strategy: dict[str, Any]) -> Any:
    cls = load_class(strategy["python"])
    from_params = getattr(cls, "from_params", None)
    params = strategy.get("params", {})
    return from_params(params) if callable(from_params) else cls(**params)


def _set_path(cfg: dict[str, Any], key: str, value: Any) -> None:
    node = cfg
    parts = key.split(".")
    for part in parts[:-1]:
        child = node.get(part)
        if not isinstance(child, dict):
            child = {}
            node[part] = child
        node = child
    node[parts[-1]] = copy.deepcopy(value)


def _short(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:.6g}"
    return v if isinstance(v, str) else json.dumps(v, separators=(",", ":"))
