"""Python strategies in the simulator (P4.3, P4.4): SimConfig, Simulator, the adapter, callback modes, the GIL."""

import threading
from pathlib import Path

import pytest

import blocksprint_sim as bs  # built by uv sync; a missing module is a failure, not a skip

FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "sim" / "data" / "btcusdt_2026-11-05_0200_1m.bse"
SECOND = 1_000_000_000


def config(tmp_path, *overrides, **extra):
    base = {"dataset": str(FIXTURE), "latency_preset": "binance_spot_tokyo_vps", "strategy": {"name": "null"},
            "output": {"dir": str(tmp_path), "sample_ms": 1000}, **extra}
    return bs.SimConfig(base, list(overrides))


class TimerQuoter:
    """Every second: a post-only bid at the best bid. Counts the callbacks it gets."""

    def __init__(self):
        self.calls = {"start": 0, "book": 0, "trade": 0, "timer": 0, "exec": 0, "end": 0}
        self.final = None

    def on_start(self, ctx):
        self.calls["start"] += 1
        ctx.set_timer(SECOND)

    def on_book(self, ctx, book):
        self.calls["book"] += 1

    def on_trade(self, ctx, trade):
        self.calls["trade"] += 1

    def on_timer(self, ctx, timer_id):
        self.calls["timer"] += 1
        book = ctx.book()
        if ctx.can_trade() and book.valid():
            ctx.send(bs.Side.BUY, bs.OrderType.LIMIT_MAKER, book.best_bid().price, bs.to_fixed("0.001"))
        ctx.set_timer(SECOND)

    def on_exec(self, ctx, report):
        self.calls["exec"] += 1

    def on_end(self, ctx):
        self.calls["end"] += 1
        self.final = ctx.account()


def test_values_and_config(tmp_path):
    assert bs.to_fixed("100.01") == 10_001_000_000
    assert bs.to_fixed("-0.5") == -50_000_000
    assert bs.to_fixed(2) == 2 * bs.FIXED_SCALE
    assert bs.from_fixed(10_001_000_000) == "100.01"
    with pytest.raises(ValueError):
        bs.to_fixed("0.000000001")  # 9 places
    cfg = config(tmp_path, "seed=3")
    assert cfg.symbol == "BTCUSDT" and cfg.strategy_name == "null"
    assert cfg.resolved["seed"] == 3
    assert cfg.data_file.endswith("btcusdt_2026-11-05_0200_1m.bse")


def test_callback_modes_filter_book_and_trade_callbacks_only(tmp_path):
    runs = {}
    for mode in ("every_event", "on_change", "timer_only"):
        s = TimerQuoter()
        meta = bs.Simulator(config(tmp_path / mode), s, mode).run()
        runs[mode] = (s, meta)
    every, change, timer = (runs[m][0] for m in ("every_event", "on_change", "timer_only"))
    assert every.calls["book"] > change.calls["book"] > 0 == timer.calls["book"]
    assert every.calls["trade"] == change.calls["trade"] > 0 == timer.calls["trade"]
    for s in (every, change, timer):
        assert s.calls["start"] == s.calls["end"] == 1
        assert s.calls["timer"] == 59 and s.calls["exec"] > 0
    # The strategy acts only on its timer, so every mode trades the same: same results, same hash
    hashes = {m: runs[m][1]["output_hash"] for m in runs}
    assert len(set(hashes.values())) == 1
    assert every.final.fills == timer.final.fills > 0
    meta = runs["every_event"][1]
    assert meta["sessions"][0]["strategy"] == "TimerQuoter"
    assert meta["python_strategy"] == "TimerQuoter"
    assert meta["python_callbacks"] == sum(every.calls.values())


def test_a_python_strategy_doing_nothing_matches_the_cpp_null_strategy(tmp_path):
    class Nothing:
        name = "nothing"

    py_meta = bs.Simulator(config(tmp_path / "py"), Nothing()).run()
    cpp_meta = bs.Simulator(config(tmp_path / "cpp")).run()
    assert py_meta["output_hash"] == cpp_meta["output_hash"]
    assert py_meta["sessions"][0]["strategy"] == "nothing" and py_meta["python_callbacks"] == 0


def test_errors(tmp_path):
    class Boom:
        def on_start(self, ctx):
            ctx.set_timer(5 * SECOND)

        def on_timer(self, ctx, timer_id):
            raise KeyError("strategy bug")

    with pytest.raises(KeyError, match="strategy bug"):
        bs.Simulator(config(tmp_path / "boom"), Boom()).run()

    class Early:
        def on_start(self, ctx):
            ctx.send(bs.Side.BUY, bs.OrderType.LIMIT, bs.to_fixed("1"), bs.to_fixed("0.001"))

    with pytest.raises(RuntimeError, match="warm-up"):
        bs.Simulator(config(tmp_path / "early", "window.warmup_s=5"), Early()).run()

    sim = bs.Simulator(config(tmp_path / "once"))
    sim.run()
    with pytest.raises(RuntimeError, match="runs once"):
        sim.run()
    with pytest.raises(ValueError, match="callback_mode"):
        bs.Simulator(config(tmp_path), TimerQuoter(), "sometimes")


def test_the_gil_is_released_during_the_replay(tmp_path):
    # While a run with a C++ strategy replays in another thread, this thread keeps running Python. If the
    # replay held the GIL, the loop below couldn't run until it finished.
    cfg = config(tmp_path, strategy={"name": "passive_quoter", "params": {"refresh_ms": 10}})
    sim = bs.Simulator(cfg)
    worker = threading.Thread(target=sim.run)
    spins = 0
    worker.start()
    while worker.is_alive():
        spins += 1
    worker.join()
    assert spins > 10_000
