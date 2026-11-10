"""A run's results in Python (P4.5): numpy structured arrays, and polars DataFrames that equal the CSVs.

    from research.sim.results import Results

    res = Results.run(config, MyStrategy(), callback_mode="timer_only")   # or Results.from_simulator(sim)
    res.exec_reports["last_qty"].sum()        # numpy, raw units, no copy
    frames = res.to_polars()                  # dict of DataFrames, one per CSV
    frames["positions"].select("ts_ns", "pnl")

numpy arrays (`orders`, `exec_reports`, `request_rejects`, `bbo_samples`, `positions`) view the simulator's
own rows: times in ns since the epoch, prices / quantities / money as integers in 1e-8 units, enums as codes
(`codes` names them), `owner` indexing `owners` and `order` indexing `order_ids`. A field the CSV leaves
empty holds 0 there; the rules are in src/sim/recorder.hpp.

`to_polars()` gives the CSV tables (docs/simulator_architecture.md section 12): the same columns in the same
order, with the CSV's strings for enums, owners and order IDs, nulls where the CSV is empty, `*_ns` times as
UTC datetimes and fixed-point values as exact Decimal(38, 8) (or floats / raw integers on request).
"""

from __future__ import annotations

from typing import Any, Literal

import numpy as np

import blocksprint_sim as bs

TABLES = ("orders", "exec_reports", "request_rejects", "bbo_samples", "positions")
NO_OWNER = 0xFFFFFFFF  # bbo_samples of a run without strategies

Fixed = Literal["decimal", "float", "int"]

# The fixed-point columns of each table (1e-8 units)
_FIXED = {
    "orders": ("price", "quantity", "new_price"),
    "exec_reports": ("price", "quantity", "last_px", "last_qty", "cum_qty", "leaves_qty", "fee", "cum_fee"),
    "request_rejects": (),
    "bbo_samples": tuple(f"{src}_{side}{qty}" for src in ("data", "book", "seen") for side in ("bid", "ask")
                         for qty in ("", "_qty")),
    "positions": ("position", "cash", "fees", "mark", "pnl", "base_volume", "quote_volume"),
}

# CSV column order (src/sim/recorder.cpp)
_COLUMNS = {
    "orders": ("sent_ns", "arrive_ns", "owner", "request", "order_id", "side", "order_type", "price", "quantity",
               "new_price", "stp"),
    "exec_reports": ("exchange_ns", "deliver_ns", "owner", "seq", "order_id", "type", "reason", "side", "order_type",
                     "price", "quantity", "last_px", "last_qty", "cum_qty", "leaves_qty", "liquidity", "fee",
                     "cum_fee"),
    "request_rejects": ("exchange_ns", "deliver_ns", "owner", "request", "order_id", "reason", "text"),
    "bbo_samples": ("ts_ns", "owner", "data_bid", "data_bid_qty", "data_ask", "data_ask_qty", "book_bid",
                    "book_bid_qty", "book_ask", "book_ask_qty", "seen_bid", "seen_bid_qty", "seen_ask", "seen_ask_qty",
                    "seen_data_ns"),
    "positions": ("ts_ns", "owner", "position", "cash", "fees", "mark", "pnl", "fills", "maker_fills", "taker_fills",
                  "open_orders", "base_volume", "quote_volume"),
}


class Results:
    """One run's outputs. `meta` is run_meta.json; the numpy tables are attributes named after the CSVs."""

    def __init__(self, meta: dict[str, Any], tables: dict[str, Any]):
        self.meta = meta
        self.owners: list[str] = tables["owners"]
        self.order_ids: list[str] = tables["order_ids"]
        self.reject_texts: list[str] = tables["reject_texts"]
        self.orders: np.ndarray = tables["orders"]
        self.exec_reports: np.ndarray = tables["exec_reports"]
        self.request_rejects: np.ndarray = tables["request_rejects"]
        self.bbo_samples: np.ndarray = tables["bbo_samples"]
        self.positions: np.ndarray = tables["positions"]
        self.codes: dict[str, list[str]] = bs.result_codes()

    @classmethod
    def from_simulator(cls, sim: bs.Simulator) -> Results:
        """The results of a Simulator that has run."""
        if sim.meta is None:
            raise RuntimeError("Results.from_simulator: run the simulator first")
        return cls(sim.meta, sim.tables())

    @classmethod
    def run(cls, config: bs.SimConfig | dict[str, Any], strategy: Any = None, callback_mode: str = "every_event",
            overrides: list[str] | tuple[str, ...] = ()) -> Results:
        """Runs one simulation (bs.Simulator) and returns its results. config: a SimConfig, or a dict."""
        if not isinstance(config, bs.SimConfig):
            config = bs.SimConfig(config, list(overrides))
        elif overrides:
            raise ValueError("Results.run: overrides go with a config dict, not a SimConfig")
        sim = bs.Simulator(config, strategy, callback_mode)
        sim.run()
        return cls.from_simulator(sim)

    @property
    def output_hash(self) -> str:
        return self.meta["output_hash"]

    def __repr__(self) -> str:
        counts = ", ".join(f"{t} {len(getattr(self, t))}" for t in TABLES)
        return f"Results({counts}; output_hash {self.output_hash[:12]})"

    def to_polars(self, table: str | None = None, *, fixed: Fixed = "decimal"):
        """The CSV tables as polars DataFrames: a dict of all five, or the one named.

        fixed: "decimal" (exact Decimal(38, 8), the default), "float" (Float64, for plotting) or "int" (the raw
        Int64 in 1e-8 units)."""
        if fixed not in ("decimal", "float", "int"):
            raise ValueError(f"fixed must be 'decimal', 'float' or 'int', not {fixed!r}")
        if table is not None:
            if table not in TABLES:
                raise ValueError(f"unknown table {table!r}: one of {', '.join(TABLES)}")
            return self._frame(table, fixed)
        return {t: self._frame(t, fixed) for t in TABLES}

    # ---- conversion ----

    def _frame(self, table: str, fixed: Fixed):
        import polars as pl

        a: np.ndarray = getattr(self, table)
        valid = _valid_masks(table, a, self.codes)
        cols: dict[str, pl.Series] = {}
        for name in _COLUMNS[table]:
            mask = valid.get(name)
            if name == "owner":
                cols[name] = _strings(name, a["owner"], self.owners, a["owner"] != NO_OWNER)
            elif name == "order_id":
                cols[name] = _strings(name, a["order"], self.order_ids, None)
            elif name == "text":
                cols[name] = pl.Series(name, self.reject_texts, dtype=pl.String)
            elif name in ("request", "side", "order_type", "stp", "type", "reason", "liquidity"):
                key = "reject_request" if table == "request_rejects" and name == "request" else name
                cols[name] = _strings(name, a[name], self.codes[key], mask)
            elif name.endswith("_ns"):
                cols[name] = _times(name, a[name], mask)
            elif name in _FIXED[table]:
                cols[name] = _fixed(name, a[name], mask, fixed)
            else:
                cols[name] = pl.Series(name, a[name].astype(np.int64 if a[name].dtype.kind == "i" else np.uint64))
        return pl.DataFrame(cols)


def _valid_masks(table: str, a: np.ndarray, codes: dict[str, list[str]]) -> dict[str, np.ndarray]:
    """Per column, where the CSV has a value (columns missing here always have one)."""
    if table == "orders":
        req = a["request"]
        new_or_replace = (req == codes["request"].index("new")) | (req == codes["request"].index("replace"))
        return {"side": new_or_replace, "order_type": new_or_replace, "stp": new_or_replace, "price": new_or_replace,
                "quantity": req != codes["request"].index("cancel"), "new_price": a["has_new_price"] != 0}
    if table == "exec_reports":
        fill = a["type"] == codes["type"].index("fill")
        return {"reason": a["reason"] != 0, "last_px": fill, "last_qty": fill, "liquidity": fill, "fee": fill}
    if table == "request_rejects":
        return {"reason": a["reason"] != 0}
    if table == "bbo_samples":
        out = {"seen_data_ns": a["seen_data_ns"] != 0}
        for src in ("data", "book", "seen"):
            for side in ("bid", "ask"):
                has = a[f"{src}_{side}"] != 0
                out[f"{src}_{side}"] = out[f"{src}_{side}_qty"] = has
        return out
    return {}


def _strings(name: str, idx: np.ndarray, names: list[str], mask: np.ndarray | None):
    import polars as pl

    lookup = np.array(names + [""], dtype=object)
    values = lookup[np.where(idx < len(names), idx, len(names))]
    s = pl.Series(name, values, dtype=pl.String)
    return s if mask is None else _mask(s, mask)


def _times(name: str, ns: np.ndarray, mask: np.ndarray | None):
    import polars as pl

    s = pl.Series(name, ns.astype(np.int64)).cast(pl.Datetime("ns", "UTC"))
    return s if mask is None else _mask(s, mask)


def _fixed(name: str, v: np.ndarray, mask: np.ndarray | None, fixed: Fixed):
    import polars as pl

    v = v.astype(np.int64, copy=False)
    if fixed == "int":
        s = pl.Series(name, v)
    elif fixed == "float":
        s = pl.Series(name, v / 1e8)
    else:
        # Exact: an Arrow decimal128 is the unscaled integer as 16 little-endian bytes (low word, then the sign)
        import pyarrow as pa

        words = np.empty((len(v), 2), dtype=np.int64)
        words[:, 0] = v
        words[:, 1] = v >> 63
        arr = pa.Array.from_buffers(pa.decimal128(38, 8), len(v), [None, pa.py_buffer(words)])
        s = pl.Series(name, arr)
    return s if mask is None else _mask(s, mask)


def _mask(s, mask: np.ndarray):
    import polars as pl

    return pl.select(pl.when(pl.Series(mask)).then(s).otherwise(None).alias(s.name)).to_series()
