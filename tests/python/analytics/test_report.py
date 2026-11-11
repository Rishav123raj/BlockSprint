"""Tearsheets and comparison reports (P5.9): content, self-containment, determinism, the CLI."""

import re
from pathlib import Path

import numpy as np
import pytest

from research.analytics import report
from research.analytics.pnl import decompose
from research.experiments.runner import run_experiment
from research.experiments.schema import Experiment
from research.run import main as run_main
from research.sim.results import Results

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
QUOTER = {"name": "passive_quoter", "params": {"size": "0.001", "refresh_ms": 100, "max_position": "0.005"}}


def experiment(tmp_path, **extra):
    data = {"name": "rep", "dataset": str(FIXTURE), "seeds": [1], "latency_preset": "zero",
            "results": str(tmp_path / "results"), "strategy": QUOTER,
            "sweep": {"params": {"latency_preset": ["zero", "binance_spot_tokyo_vps"],
                                 "queue_model": [{"model": "cancel_from_back"}, {"model": "pro_rata"}]}}}
    data.update(extra)
    return Experiment.model_validate(data)


def self_contained(page: str) -> bool:
    """No external resources: nothing fetched from a URL, no external scripts or stylesheets"""
    return not re.search(r"""(src|href)=["']?(https?:)?//""", page) and "<link" not in page and "@import" not in page


def test_tearsheet_content(tmp_path):
    res = Results.run({"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps", "strategy": QUOTER,
                       "output": {"dir": str(tmp_path / "out"), "sample_ms": 1000}})
    page = report.tearsheet(res)
    assert page.startswith("<!doctype html>") and page.rstrip().endswith("</html>")
    assert self_contained(page)
    for heading in ("PnL, mark-to-mid", "Drawdown from the running high", "PnL split, cumulative", "Position",
                    "Markouts", "Fill rate by queue ahead", "Transaction costs", "Risk"):
        assert heading in page
    d = decompose(res).row(0, named=True)
    assert report.fmt(d["pnl"], 2, signed=True) in page  # the headline PnL
    assert "1s buckets" in page  # one minute of data: 1 s buckets (at least 30)
    assert page.count("<svg") == 6 and page.count("<table>") >= 6  # every chart has its table
    assert report.tearsheet(res) == page  # no times of its own


def test_experiment_reports_are_identical_on_a_cache_hit(tmp_path):
    exp = experiment(tmp_path)
    first = run_experiment(exp, workers=1, progress=None)
    path = report.write_reports(first, exp, progress=None)
    out = path.parent
    pages = sorted(out.glob("runs/*.html"))
    assert len(pages) == 4 and {p.stem for p in pages} == {r.run_id for r in first.runs}
    page = path.read_text(encoding="utf-8")
    assert self_contained(page)
    for p in pages:
        assert f'href="runs/{p.name}"' in page and 'href="../report.html"' in p.read_text(encoding="utf-8")
    # Two swept parameters: one heatmap per metric, cells for 2 x 2 points
    assert page.count("<svg") == len(report.METRICS) and page.count('class="mark"') == 4 * len(report.METRICS)
    before = {p: p.read_bytes() for p in [path, *pages]}

    again = run_experiment(exp, workers=1, progress=None)
    assert {r.status for r in again.runs} == {"cached"}
    report.write_reports(again, exp, progress=None)
    assert {p: p.read_bytes() for p in before} == before


def test_one_swept_parameter_and_failed_runs(tmp_path):
    exp = experiment(tmp_path, name="one", sweep={"params": {"strategy.params.refresh_ms": [100, 200, 400]}})
    result = run_experiment(exp, workers=1, progress=None)
    result.runs[0].status, result.runs[0].error = "failed", "RuntimeError: boom"
    page = report.write_reports(result, exp, progress=None).read_text(encoding="utf-8")
    assert "1 failed" in page and "RuntimeError: boom" in page
    assert ">refresh_ms<" in page  # the parameter, without its strategy.params. prefix
    # One row of three cells per metric; the failed run's is empty
    assert page.count('class="mark"') == 3 * len(report.METRICS) and page.count('fill="transparent" stroke') == 4
    assert len(list((tmp_path / "results" / "experiments" / "one" / "runs").glob("*.html"))) == 2


def test_cli_writes_reports_unless_told_not_to(tmp_path):
    cfg = tmp_path / "exp.yaml"
    cfg.write_text(f"name: cli\ndataset: {FIXTURE.as_posix()}\nlatency_preset: zero\n"
                   f"results: {(tmp_path / 'results').as_posix()}\nstrategy: {{name: passive_quoter}}\n",
                   encoding="utf-8")
    out = tmp_path / "results" / "experiments" / "cli"
    assert run_main([str(cfg), "--workers", "1", "--no-report"]) == 0
    assert not (out / "report.html").exists()
    assert run_main([str(cfg), "--workers", "1"]) == 0
    assert (out / "report.html").exists() and len(list((out / "runs").glob("*.html"))) == 1
    run_id = next((out / "runs").glob("*.html")).stem
    target = tmp_path / "one.html"
    assert report.main([run_id, "--results", str(tmp_path / "results"), "--out", str(target)]) == 0
    assert target.read_bytes() != b"" and "PnL, mark-to-mid" in target.read_text(encoding="utf-8")


def test_formatting_and_scales():
    assert report.fmt(-0.0) == "0.00" and report.fmt(1234.5, 1) == "1,234.5"
    assert report.fmt(2, signed=True) == "+2" and report.fmt(float("nan")) == "-" and report.fmt(None) == "-"
    assert report._nice_ticks(-1.57, 0.0) == [-2.0, -1.5, -1.0, -0.5, 0.0]
    assert report.auto_every(60 * 10**9) == "1s" and report.auto_every(3600 * 10**9) == "1m"
    assert report.auto_every(6 * 3600 * 10**9) == "5m"
    # Diverging: gray at zero, the poles at the extremes; sequential: the ramp's ends
    assert report._cell_color(0.0, -2, 2, True) == "#f0efec"
    assert report._cell_color(2.0, -2, 2, True) == "#2a78d6" and report._cell_color(-2.0, -2, 2, True) == "#e34948"
    assert report._cell_color(0.0, 0, 5, False) == "#cde2fb" and report._cell_color(5.0, 0, 5, False) == "#0d366b"
    assert report._cell_color(float("nan"), 0, 1, False) == "transparent"
    # Labels are escaped
    chart = report.bar_chart(["<b>"], [1.0], y_title="t")
    assert "<b>" not in chart and "&lt;b&gt;" in chart
    grid = report.heatmap("x", ["a", "b"], "y", ["c"], np.array([[1.0, np.nan]]), title="t")
    assert grid.count('class="mark"') == 2
    # Signed values all below zero: one red hue over their range (the closest to 0 lightest), not a scale
    # centred on 0 that would paint close values alike
    losses = report.heatmap("x", ["a", "b"], "y", ["c"], np.array([[-84.0, -83.0]]), title="t", diverging=True)
    assert report._RED[2] in losses and report._RED[0] in losses and report._MID not in losses
    mixed = report.heatmap("x", ["a", "b"], "y", ["c"], np.array([[-1.0, 2.0]]), title="t", diverging=True)
    assert report._MID in mixed
