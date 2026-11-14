"""The v2 soak test's measures and verdict (P7.8), on hand-made checks."""

import os

import pytest

from research.live.soak import crossed, growth_mb_per_h, level_match, rss_mb, verdict

BINANCE = {"bids": [["100.00", "1.0"], ["99.99", "2.0"]], "asks": [["100.01", "1.5"], ["100.02", "0.5"]]}


def test_level_match_counts_price_and_quantity():
    server = {"bids": [["100.000000", "1.000000"], ["99.990000", "1.000000"]],
              "asks": [["100.010000", "1.500000"], ["100.030000", "0.500000"]]}
    assert level_match(BINANCE, server) == {"qty": 0.5, "price": 0.75}
    assert level_match(BINANCE, BINANCE) == {"qty": 1.0, "price": 1.0}


def test_crossed():
    assert not crossed(BINANCE)
    assert crossed({"bids": [["100.02", "1"]], "asks": [["100.01", "1"]]})
    assert not crossed({"bids": [], "asks": [["1", "1"]]})


def test_growth_is_fitted_on_the_second_half():
    t = [0, 3600, 7200, 10800, 14400]
    assert growth_mb_per_h(t, [10, 50, 60, 60, 60]) == pytest.approx(0.0)   # start-up growth, then flat
    assert growth_mb_per_h(t, [10, 20, 30, 40, 50]) == pytest.approx(10.0)
    assert growth_mb_per_h(t[:2], [1, 2]) is None


def _check(t, qty, price=1.0, crossed_=False, mem=(30.0, 60.0)):
    return {"t_s": t, "symbols": {"BTCUSDT": {"synced": True, "match": {"qty": qty, "price": price},
                                              "crossed": crossed_}},
            "memory_mb": {"server": mem[0], "bridge": mem[1]}}


def test_verdict_passes_a_clean_day_with_a_reconnect():
    checks = [_check(i * 3600, 1.0 if i % 3 else 0.8) for i in range(26)]
    v = verdict(checks, [{"symbol": "BTC-USDT", "seconds": 4.0, "lost_at_s": 86400}], reconnects=1, hours=26,
                died=None)
    assert v["pass"] and all(v["criteria"].values()) and v["covers_24h_reconnect"]
    assert v["symbols"]["BTCUSDT"]["median_match"] == 1.0 and v["slowest_resync_s"] == 4.0


def test_verdict_failures():
    good = [_check(i * 60, 1.0) for i in range(10)]
    short = verdict(good, [], reconnects=0, hours=2, died=None)
    assert all(short["criteria"].values()) and not short["covers_24h_reconnect"] and not short["pass"]
    stuck = good[:3] + [_check(300 + i, 0.2, price=0.3) for i in range(5)] + good[3:]
    assert not verdict(stuck, [], 1, 26, None)["criteria"]["book_matches"]
    assert not verdict(good + [_check(999, 1.0, crossed_=True)], [], 1, 26, None)["criteria"]["never_crossed"]
    assert not verdict(good, [{"symbol": "X", "seconds": 45.0, "lost_at_s": 1}], 1, 26, None)["criteria"]["resyncs_fast"]
    assert not verdict(good, [{"symbol": "X", "seconds": None, "lost_at_s": 1}], 1, 26, None)["criteria"]["resyncs_fast"]
    leak = [_check(i * 3600, 1.0, mem=(30.0 + 50 * i, 60.0)) for i in range(10)]
    assert not verdict(leak, [], 1, 26, None)["criteria"]["memory_levels_off"]
    died = verdict(good, [], 1, 26, "bridge")
    assert not died["criteria"]["processes_up"] and died["died"] == "bridge"


def test_rss_of_this_process():
    mb = rss_mb(os.getpid())
    assert mb is not None and mb > 1


def test_tree_memory_includes_children():
    import subprocess
    import sys
    import time

    from research.live.soak import _children, tree_rss_mb

    child = subprocess.Popen([sys.executable, "-c", "import time; x = bytearray(50 * 2**20); time.sleep(30)"])
    try:
        time.sleep(3)
        assert child.pid in _children(os.getpid())
        assert tree_rss_mb(os.getpid()) >= rss_mb(os.getpid()) + 40
    finally:
        child.kill()
