"""Calibration (P7.6): on a session whose "paper" side is a simulator run at a known exponent and latency, the grid
search must find that point, at zero loss."""

import json
from pathlib import Path

import pytest
from paper_fixture import make_session

from research.calibration.calibrate import calibrate, fitted_latency, label, scaled_latency, score
from research.live.paper_session import default_latency


def test_scaled_latency_scales_the_measured_delays():
    meta = {"measured_latency": {"md_delay_ms": {"mean": 40.0, "p10": 10.0, "p90": 70.0}, "order_rtt_ms": {"p50": 4.0}}}
    base = default_latency(meta)
    assert scaled_latency(meta, 1, 1) == base
    lat = scaled_latency(meta, 0.5, 3)
    assert lat["feed"]["mean_us"] == 20000 and lat["feed"]["sd_us"] == round(base["feed"]["sd_us"] * 0.5)
    assert lat["entry"] == lat["ack"] == {"model": "constant", "us": 6000}


def test_score_is_in_units_of_the_tolerances():
    reports = [{"checks": {"fill_ratio_rel": {"value": 0.15, "pass": True},
                           "markout_1s_diff_ticks": {"value": -2.0, "tolerance": 3.0, "seed_range": 3.0, "pass": True}}},
               {"checks": {"fill_ratio_rel": {"value": 0.0, "pass": True},
                           "markout_1s_diff_ticks": {"value": 4.0, "tolerance": 1.0, "seed_range": None, "pass": False}}}]
    s = score(reports, markout_scale=2.0)
    assert s["loss"] == pytest.approx(((1 + 1) + (0 + 4)) / 2)
    assert s["sessions_passed"] == 1 and not s["accepted"]


def test_the_grid_search_finds_the_settings_that_made_the_paper_side(tmp_path):
    session = make_session(tmp_path, queue_model="prob_power:2")
    result = calibrate([session], ns=[1, 2, 4], feed_scales=[1.0, 20.0], order_scales=[1.0], seeds=[1], baseline_n=2)
    best = result["best"]
    assert (best["n"], best["feed_scale"], best["order_scale"]) == (2, 1.0, 1.0)
    assert best["loss"] == pytest.approx(0.0) and best["sessions_passed"] == 1
    assert len(result["points"]) == 6 and result["points"][-1]["loss"] > 0
    assert (session / "calib" / label(2, 1.0, 1.0) / "replay.json").exists()
    meta = json.loads((session / "paper" / "session_meta.json").read_text(encoding="utf-8"))
    assert fitted_latency([session], 1.0, 1.0) == scaled_latency(meta, 1.0, 1.0)


def test_the_calibrated_profile_is_a_valid_run_config():
    import blocksprint_sim as bs
    from paper_fixture import FIXTURE

    profile = Path(__file__).resolve().parents[2] / "configs" / "sim" / "calibrated" / "paper_btcusdt.json"
    cfg = json.loads(profile.read_text(encoding="utf-8"))
    resolved = bs.resolve_config({**cfg, "dataset": str(FIXTURE), "strategy": {"name": "null"}})
    assert resolved["queue_model"] == {"model": "prob_power:1.5"}
    assert resolved["latency"] == cfg["latency"]
