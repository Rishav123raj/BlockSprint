# Notebooks

| Notebook | What it shows |
| :--- | :--- |
| `01_data_exploration.ipynb` | A day of BTCUSDT through the simulator's eyes: the spread (share of time by ticks), the mid, traded volume by aggressor side, trade sizes, and whether the top-of-book imbalance leads the next second's move |
| `02_strategy_walkthrough.ipynb` | Write a strategy in the notebook (a quoter that skips the side the book leans against), run it with `Results.run`, compare it with the C++ `passive_quoter`: PnL split, TCA, markouts, queue, risk, then its tearsheet |
| `03_sensitivity_study.ipynb` | An experiment defined in code: the Avellaneda-Stoikov market maker over latency × queue model, with heatmaps of fills, PnL and markout, the spread of the answers, and the comparison report |

```powershell
uv run --with jupyterlab jupyter lab research/notebooks
```

Jupyter isn't a project dependency; `uv run --with` brings it for the session.

**Data:**
- **The default:** every notebook runs on the committed one-minute slice, in a few seconds.
- **The recorded day:** change the first code cell (the lines are there, commented). This needs the packed dataset `data/packed/binance_spot_btc_eth_1110`, which isn't in git ([docs/data_pipeline.md](../../docs/data_pipeline.md)).

**Charts:**
- They are the tearsheets' charts (`research.analytics.report`), shown with `display(report.frame(...))`: a self-contained frame with hover read-outs and a table under each chart.
- No plotting library is needed.

**Committing changes:** save notebooks without outputs (*Edit → Clear Outputs of All Cells*) to keep diffs small. `tests/python/test_notebooks.py` checks that they are saved clean and runs every code cell top to bottom on the slice.
