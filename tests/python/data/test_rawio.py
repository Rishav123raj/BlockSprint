import json

from research.data.rawio import RawWriter, archive_files, hour_key, make_envelope, read_raw

# 2026-11-02 15:59:59 UTC and one second later
T_1559 = 1793635199_000000000
T_1600 = 1793635200_000000000


class FakeClock:
    def __init__(self, t: int = T_1559) -> None:
        self.t = t

    def __call__(self) -> int:
        return self.t


def test_hour_key_is_utc():
    assert hour_key(T_1559) == ("2026-11-02", "15")
    assert hour_key(T_1600) == ("2026-11-02", "16")


def test_make_envelope_omits_unset_fields():
    assert make_envelope(5, "snapshot", "{}", t0=4) == {"t": 5, "k": "snapshot", "t0": 4, "d": "{}"}
    assert make_envelope(5, "ws", "x", conn=2, stream="btcusdt@trade") == {
        "t": 5, "k": "ws", "c": 2, "s": "btcusdt@trade", "d": "x"}


def test_round_trip_and_hourly_rotation(tmp_path):
    clock = FakeClock()
    with RawWriter(tmp_path, clock=clock) as w:
        w.write("BTCUSDT", make_envelope(T_1559, "ws", '{"a":1}', conn=1, stream="btcusdt@trade"))
        w.write("BTCUSDT", make_envelope(T_1559 + 5, "ws", '{"a":2}', conn=1, stream="btcusdt@trade"))
        w.write("ETHUSDT", make_envelope(T_1559 + 7, "snapshot", '{"lastUpdateId":9}', t0=T_1559))
        clock.t = T_1600
        w.write("BTCUSDT", make_envelope(T_1600, "ws", '{"a":3}', conn=1, stream="btcusdt@trade"))

    btc = archive_files(tmp_path, "BTCUSDT")
    assert [p.parent.name for p in btc] == ["2026-11-02", "2026-11-02"]
    assert [p.name.split("-")[0] for p in btc] == ["15", "16"]
    assert all(p.name.endswith(".jsonl.zst") for p in btc)  # finalized, no .part left
    assert [json.loads(e["d"])["a"] for p in btc for e in read_raw(p)] == [1, 2, 3]

    eth = archive_files(tmp_path, "ETHUSDT")
    assert [e["k"] for e in read_raw(eth[0])] == ["snapshot"]
    assert archive_files(tmp_path, "XRPUSDT") == []


def test_part_file_is_readable_after_flush_without_close(tmp_path):
    """A crash leaves a .part file; everything flushed before the crash must be readable."""
    clock = FakeClock()
    w = RawWriter(tmp_path, clock=clock, flush_interval_s=1.0)
    w.write("BTCUSDT", make_envelope(T_1559, "ws", "first", conn=1))
    w.flush()
    w.write("BTCUSDT", make_envelope(T_1559 + 1, "ws", "second", conn=1))
    w.flush()
    w.write("BTCUSDT", make_envelope(T_1559 + 2, "ws", "unflushed", conn=1))  # lost in the "crash"

    parts = archive_files(tmp_path, "BTCUSDT")
    assert len(parts) == 1 and parts[0].name.endswith(".part")
    assert [e["d"] for e in read_raw(parts[0])] == ["first", "second"]
    w.close()
    assert [e["d"] for e in read_raw(archive_files(tmp_path, "BTCUSDT")[0])] == ["first", "second", "unflushed"]


def test_truncated_part_file_stops_cleanly(tmp_path):
    clock = FakeClock()
    w = RawWriter(tmp_path, clock=clock)
    for i in range(50):
        w.write("BTCUSDT", make_envelope(T_1559 + i, "ws", f"frame-{i}", conn=1))
        w.flush()
    part = archive_files(tmp_path, "BTCUSDT")[0]
    data = part.read_bytes()
    part.write_bytes(data[: len(data) - 7])  # cut into the last frame, as a crash mid-write would

    frames = [e["d"] for e in read_raw(part)]
    assert frames == [f"frame-{i}" for i in range(len(frames))]
    assert 40 <= len(frames) < 50
    # The writer's handle refers to the truncated file: release it without finalizing
    w._files.pop("BTCUSDT").handle.close()


def test_write_after_close_raises(tmp_path):
    import pytest

    w = RawWriter(tmp_path, clock=FakeClock())
    w.write("BTCUSDT", make_envelope(T_1559, "ws", "a", conn=1))
    w.close()
    with pytest.raises(RuntimeError):
        w.write("BTCUSDT", make_envelope(T_1559 + 1, "ws", "late", conn=1))
    assert all(not p.name.endswith(".part") for p in archive_files(tmp_path, "BTCUSDT"))


def test_time_based_flush(tmp_path):
    clock = FakeClock()
    w = RawWriter(tmp_path, clock=clock, flush_interval_s=5.0)
    w.write("BTCUSDT", make_envelope(T_1559, "ws", "a", conn=1))
    part = archive_files(tmp_path, "BTCUSDT")[0]
    assert list(read_raw(part)) == []  # not flushed yet
    clock.t += 5_000_000_000
    w.maybe_flush()
    assert [e["d"] for e in read_raw(part)] == ["a"]
    w.close()
