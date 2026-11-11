"""Notebooks (P5.11): each one is a valid, clean notebook whose code runs top to bottom on the committed slice.

Jupyter isn't a dependency: the cells run here in one namespace, with IPython.display stood in for when IPython
isn't installed (what they display is checked to be a chart frame).
"""

import json
import os
import sys
import types
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
NOTEBOOKS = sorted((ROOT / "research" / "notebooks").glob("*.ipynb"))


def test_the_three_notebooks_exist():
    assert [p.name for p in NOTEBOOKS] == ["01_data_exploration.ipynb", "02_strategy_walkthrough.ipynb",
                                           "03_sensitivity_study.ipynb"]


@pytest.fixture
def ipython_display(monkeypatch):
    shown = []
    try:
        import IPython.display  # noqa: F401
    except ImportError:
        display = types.ModuleType("IPython.display")
        display.display = lambda *objs: shown.extend(objs)
        package = types.ModuleType("IPython")
        package.display = display
        monkeypatch.setitem(sys.modules, "IPython", package)
        monkeypatch.setitem(sys.modules, "IPython.display", display)
    return shown


@pytest.mark.parametrize("path", NOTEBOOKS, ids=lambda p: p.stem)
def test_notebook_runs(path, tmp_path, monkeypatch, ipython_display):
    nb = json.loads(path.read_text(encoding="utf-8"))
    assert nb["nbformat"] == 4 and nb["cells"][0]["cell_type"] == "markdown"
    code = [c for c in nb["cells"] if c["cell_type"] == "code"]
    assert code and all(c["outputs"] == [] and c["execution_count"] is None for c in code)  # saved clean
    monkeypatch.setenv("BS_RESULTS", str(tmp_path / "results"))
    monkeypatch.setenv("BS_WORKERS", "1")
    monkeypatch.chdir(ROOT / "research" / "notebooks")  # as Jupyter opens it
    ns = {"__name__": "__main__"}
    for i, cell in enumerate(code):
        source = "".join(cell["source"])
        try:
            exec(compile(source, f"{path.name}[{i}]", "exec"), ns)
        except Exception as e:
            raise AssertionError(f"{path.name}, code cell {i}: {type(e).__name__}: {e}\n{source}") from e
    for shown in ipython_display:  # report.frame: an iframe holding a whole page with its charts
        assert shown._repr_html_().startswith("<iframe srcdoc=") and "&lt;svg" in shown
    if path.stem == "03_sensitivity_study":
        assert (tmp_path / "results" / "experiments" / "notebook_sensitivity" / "report.html").exists()
        assert ns["table"].height == 12
    if path.stem == "02_strategy_walkthrough":
        assert ns["mine"].meta["python_strategy"] == "ImbalanceAwareQuoter"
    os.environ.pop("BS_RESULTS", None)
