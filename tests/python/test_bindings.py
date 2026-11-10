"""blocksprint_sim, the simulator as a Python module (Phase 4): build, config path, errors.

The run test reuses a pinned scenario of tests/sim/determinism_test.cpp: the Python path must give the same
output hash as the C++ test, on every platform.
"""

import os
from pathlib import Path

import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"

# tests/sim/determinism_test.cpp, scenario "TWAP seller, home PC latency, prob_log, warm-up, orders first"
TWAP_CONFIG = {
    "latency_preset": "binance_spot_home_pc",
    "queue_model": {"model": "prob_log"},
    "seed": 11,
    "tie_break": "orders_first",
    "window": {"warmup_s": 2},
    "strategy": {"name": "twap_taker",
                 "params": {"side": "sell", "quantity": "0.05", "slices": 12, "duration_ms": 50000}},
    "output": {"sample_ms": 500},
}
TWAP_HASH = "4a71b6e682c666e2223c7e2da9c4a8949dacee2fe2a21587ef7dce8e641f0614"


def config(tmp_path, **extra):
    return {**TWAP_CONFIG, "dataset": str(FIXTURE), "output": {**TWAP_CONFIG["output"], "dir": str(tmp_path)}, **extra}


@pytest.mark.skipif(os.environ.get("BLOCKSPRINT_EXPECT_INSTALLED") != "1",
                    reason="CI's wheel test (P4.8) sets BLOCKSPRINT_EXPECT_INSTALLED=1")
def test_the_installed_wheel_is_what_is_tested():
    # When CI tests a built wheel, both packages must come from the wheel, not from the source tree
    import research

    for module in (bs, research):
        path = Path(module.__file__).resolve()
        assert "site-packages" in path.parts, path
        assert ROOT not in path.parents, path


def test_module_basics():
    assert bs.__version__ == "2.0.0"
    assert {"null", "passive_quoter", "twap_taker"} <= set(bs.strategy_names())
    assert "zero" in bs.latency_presets()


def test_resolve_config_applies_layers_and_overrides(tmp_path):
    r = bs.resolve_config(config(tmp_path), ["seed=5", "window.from=2026-11-05T02:00:10Z"])
    assert r["seed"] == 5
    assert r["window"]["from"] == "2026-11-05T02:00:10.000000000Z"
    assert r["latency"]["entry"]["model"] == "lognormal"   # from the preset
    assert r["fees"]["maker_rate"] == "0.001"               # a default
    # JSON text works too, and resolving the resolved config changes nothing
    import json
    assert bs.resolve_config(json.dumps(r)) == r


def test_run_matches_the_cpp_pinned_hash(tmp_path):
    meta = bs.run(config(tmp_path))
    assert meta["output_hash"] == TWAP_HASH
    session = meta["sessions"][0]
    assert session["strategy"] == "twap_taker"
    assert session["exchange_account"]["fills"] == 12
    assert (tmp_path / "run_meta.json").exists() and (tmp_path / "output_hash").read_text().strip() == TWAP_HASH
    assert meta["run"]["command"] == "blocksprint_sim.run"


def test_errors_are_value_errors_naming_the_key(tmp_path):
    with pytest.raises(ValueError, match="frobnicate"):
        bs.run(config(tmp_path, frobnicate=1))
    with pytest.raises(ValueError, match="unknown strategy 'nope'"):
        bs.run({**config(tmp_path), "strategy": {"name": "nope"}})
    with pytest.raises(ValueError, match="strategy.params.sise"):
        bs.run({**config(tmp_path), "strategy": {"name": "passive_quoter", "params": {"sise": "1"}}})
