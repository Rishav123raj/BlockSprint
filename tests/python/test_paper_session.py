"""The paper session protocol's helpers (P7.4): the replay's window dates and its latency from the session's
measured delays. The protocol itself needs live Binance data; docs/parity_protocol.md has the recorded runs."""

from research.live.paper_session import _dates, default_latency


def test_dates_cover_the_session_in_utc():
    day = 1_794_528_000 * 10**9  # 2026-11-13 00:00 UTC
    assert _dates(day + 3600 * 10**9, day + 7200 * 10**9) == ["2026-11-13"]
    assert _dates(day - 60 * 10**9, day + 60 * 10**9) == ["2026-11-12", "2026-11-13"]


def test_latency_from_the_measured_delays():
    meta = {"measured_latency": {"md_delay_ms": {"mean": 47.2, "p10": 8.1, "p90": 89.1},
                                 "order_rtt_ms": {"p50": 3.5}}}
    lat = default_latency(meta)
    assert lat["feed"] == {"model": "normal", "mean_us": 47200, "sd_us": 31604, "min_us": 0}  # (89.1 - 8.1) / 2.563 = 31.6036 ms
    assert lat["entry"] == lat["ack"] == {"model": "constant", "us": 1750}                  # half the round trip
    # Nothing measured (e.g. no orders): the server's 100 ms batching and a 1 ms leg
    fallback = default_latency({})
    assert fallback["feed"]["mean_us"] == 50000 and fallback["entry"]["us"] == 1000
