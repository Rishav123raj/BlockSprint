"""Analytics on a run's results (Phase 5): the market the run saw (market), PnL and its decomposition (pnl).

Every function takes a run: research.sim.results.Results (in memory) or research.experiments.store.StoredRun
(from the store); anything with `meta` (run_meta.json) and `frame(table)` (a polars DataFrame) works too.
"""
