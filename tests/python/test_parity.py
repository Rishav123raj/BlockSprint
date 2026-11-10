"""Parity (P4.7): the Python port of PassiveQuoter (research/strategies/passive_quoter.py) and the C++ one give
identical results: the same requests at the same nanoseconds, so the same exec reports and output files.

The scenarios are tests/sim/determinism_test.cpp's PassiveQuoter ones, with their pinned output hashes, and use
random latency, the random and pro-rata queue models, a window with warm-up and plain LIMIT quotes. The last
test covers a data gap. It needs the recorded data (not in the repository) and takes about 2 minutes, so it
runs only with BLOCKSPRINT_SLOW_TESTS=1.
"""

import os
from pathlib import Path

import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip
from research.strategies.passive_quoter import PassiveQuoter

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
PACKED = ROOT / "data" / "packed" / "binance_spot_btc_eth_1105_1107"


def lognormal(median_us, sigma, min_us):
    return {"model": "lognormal", "median_us": median_us, "sigma": sigma, "min_us": min_us}


SCENARIOS = {
    "random latency, prob_power:2": ({
        "latency": {"feed": lognormal(2000, 0.5, 300),
                    "entry": {"model": "normal", "mean_us": 3000, "sd_us": 1000, "min_us": 500},
                    "ack": lognormal(2500, 0.4, 400)},
        "queue_model": {"model": "prob_power", "n": 2},
        "seed": 7,
        "strategy": {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 100, "max_position": "0.005"}},
        "output": {"sample_ms": 250},
    }, "e4983a252e78434fc0614d479c6e5ef46b05d75758e20f4ab3822b5f380c7728"),
    "Tokyo latency, pro_rata, window with warm-up, plain LIMIT": ({
        "latency_preset": "binance_spot_tokyo_vps",
        "queue_model": {"model": "pro_rata"},
        "seed": 3,
        "window": {"from": "2026-11-05T02:00:10Z", "to": "2026-11-05T02:00:50Z", "warmup_s": 5},
        "strategy": {"name": "passive_quoter",
                     "params": {"size": "0.002", "refresh_ms": 50, "post_only": False, "max_position": "0.01"}},
        "output": {"sample_ms": 100},
    }, "5df4e779a1f5b4e649c175c361cc008ee02cbffa6d4b38d98977282aeff27351"),
    # Refreshing faster than a round trip: most refreshes find a request in flight and must wait for it
    "refresh faster than the latency": ({
        "latency_preset": "binance_spot_home_pc",
        "seed": 9,
        "strategy": {"name": "passive_quoter", "params": {"refresh_ms": 3, "max_position": "0.003"}},
        "output": {"sample_ms": 500},
    }, None),
    "home PC latency, defaults": ({
        "latency_preset": "binance_spot_home_pc",
        "seed": 5,
        "strategy": {"name": "passive_quoter", "params": {}},
        "output": {"sample_ms": 1000},
    }, None),
}


def run_both(cfg, tmp_path, callback_mode=None):
    """The C++ strategy named in cfg, then its Python port with the same params, on the same config."""
    cpp = bs.Simulator(bs.SimConfig({**cfg, "output": {**cfg["output"], "dir": str(tmp_path / "cpp")}})).run()
    port = PassiveQuoter.from_params(cfg["strategy"]["params"])
    py_cfg = bs.SimConfig({**cfg, "output": {**cfg["output"], "dir": str(tmp_path / "py")}})
    py = bs.Simulator(py_cfg, port, callback_mode).run()
    return cpp, py, port


def assert_identical(cpp, py):
    for name in ("orders.csv", "exec_reports.csv", "request_rejects.csv", "bbo_samples.csv", "positions.csv"):
        assert py["outputs"][name] == cpp["outputs"][name], name
    assert py["output_hash"] == cpp["output_hash"]
    assert py["sessions"][0]["stats"] == cpp["sessions"][0]["stats"]
    assert py["sessions"][0]["strategy"] == "passive_quoter_py" and cpp["sessions"][0]["strategy"] == "passive_quoter"


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_python_port_matches_cpp(scenario, tmp_path):
    cfg, pinned = SCENARIOS[scenario]
    cpp, py, port = run_both({**cfg, "dataset": str(FIXTURE)}, tmp_path)
    assert_identical(cpp, py)
    if pinned:
        assert py["output_hash"] == pinned  # = determinism_test.cpp's C++ result
    assert py["sessions"][0]["exchange_account"]["fills"] > 0 and port.refreshes > 0


@pytest.mark.parametrize("mode", ["every_event", "on_change"])
def test_callback_mode_does_not_matter_to_a_timer_strategy(mode, tmp_path):
    cfg, pinned = SCENARIOS["random latency, prob_power:2"]
    cpp, py, _ = run_both({**cfg, "dataset": str(FIXTURE)}, tmp_path, mode)
    assert_identical(cpp, py)
    assert py["output_hash"] == pinned


def test_params_are_checked_as_in_cpp(tmp_path):
    q = PassiveQuoter.from_params({"size": "0.002", "refresh_ms": 0.0015, "max_position": 1})
    assert (q.size, q.refresh_ns, q.max_position, q.post_only) == (200_000, 1_500, 100_000_000, True)
    for params, message in [({"sise": "1"}, "unknown parameter"), ({"refresh_ms": 0}, "must be > 0"),
                            ({"size": "0.02"}, "at least size"), ({"size": "-1"}, "positive decimal"),
                            ({"size": "0.000000001"}, "positive decimal"), ({"post_only": 1}, "true or false"),
                            ({"refresh_ms": -5}, "milliseconds")]:
        with pytest.raises(ValueError, match=message):
            PassiveQuoter.from_params(params)
        # the C++ strategy refuses the same params
        base = {"dataset": str(FIXTURE), "latency_preset": "zero", "output": {"dir": str(tmp_path)}}
        bs.SimConfig({**base, "strategy": {"name": "passive_quoter", "params": {}}})  # the base itself is valid
        with pytest.raises(ValueError, match="strategy.params"):
            bs.SimConfig({**base, "strategy": {"name": "passive_quoter", "params": params}})


@pytest.mark.skipif(not (PACKED / "BTCUSDT.bse").exists(), reason="needs the recorded Nov 5-7 data (data/packed)")
@pytest.mark.skipif(os.environ.get("BLOCKSPRINT_SLOW_TESTS") != "1",
                    reason="about 2 minutes (fast-forwards 1.5 days of data twice): set BLOCKSPRINT_SLOW_TESTS=1")
def test_parity_across_a_data_gap(tmp_path):
    # BTCUSDT 2026-11-06 07:30-14:00 UTC: the recording stopped 07:40-13:45, so the quoter sees a gap
    # (on_gap: every quote cancelled), hours without data, then a fresh snapshot
    cfg = {"dataset": str(PACKED), "symbol": "BTCUSDT",
           "window": {"from": "2026-11-06T07:30:00Z", "to": "2026-11-06T14:00:00Z"},
           "latency_preset": "binance_spot_tokyo_vps", "seed": 1,
           "strategy": {"name": "passive_quoter", "params": {"refresh_ms": 250}},
           "output": {"sample_ms": 1000}}
    cpp, py, _ = run_both(cfg, tmp_path)
    assert cpp["synthesizer"]["gaps"] > 0
    assert_identical(cpp, py)
