import json

from research.data.inspect_raw import inspect_symbol, report_ok
from research.data.rawio import RawWriter, make_envelope
from research.data.recorder import build_stream_url, stream_names, symbol_from_stream

T0 = 1793635000_000000000


def test_stream_names_and_url():
    assert stream_names(["BTCUSDT", "ETHUSDT"]) == [
        "btcusdt@depth@100ms", "btcusdt@trade", "ethusdt@depth@100ms", "ethusdt@trade"]
    assert build_stream_url(["BTCUSDT"]) == (
        "wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms/btcusdt@trade")


def test_symbol_from_stream():
    assert symbol_from_stream("btcusdt@depth@100ms") == "BTCUSDT"
    assert symbol_from_stream("ethusdt@trade") == "ETHUSDT"


def _depth(conn, t, U, u):
    frame = json.dumps({"stream": "btcusdt@depth@100ms",
                        "data": {"e": "depthUpdate", "E": t // 1_000_000 - 5, "s": "BTCUSDT",
                                 "U": U, "u": u, "b": [], "a": []}})
    return make_envelope(t, "ws", frame, conn=conn, stream="btcusdt@depth@100ms")


def _trade(conn, t, trade_id):
    frame = json.dumps({"stream": "btcusdt@trade",
                        "data": {"e": "trade", "E": t // 1_000_000 - 3, "s": "BTCUSDT", "t": trade_id,
                                 "p": "1.0", "q": "1.0", "T": t // 1_000_000 - 3, "m": True}})
    return make_envelope(t, "ws", frame, conn=conn, stream="btcusdt@trade")


def _write(tmp_path, envelopes):
    with RawWriter(tmp_path) as w:
        for env in envelopes:
            w.write("BTCUSDT", env)


def test_inspect_clean_stream_with_syncable_snapshot(tmp_path):
    _write(tmp_path, [
        _depth(1, T0, 100, 105),
        _trade(1, T0 + 1, 7),
        make_envelope(T0 + 2, "snapshot", json.dumps({"lastUpdateId": 106, "bids": [], "asks": []}), t0=T0),
        _depth(1, T0 + 3, 106, 110),  # covers lastUpdateId + 1 = 107
        _trade(1, T0 + 4, 8),
        # an overlapping replacement connection re-delivers the same updates: continuity is per connection
        _depth(2, T0 + 5, 108, 110),
        _depth(2, T0 + 6, 111, 112),
    ])
    r = inspect_symbol(tmp_path, "BTCUSDT")
    assert r.connections[(0, 1)].depth_events == 2 and r.connections[(0, 1)].depth_gaps == 0
    assert r.connections[(0, 1)].trades == 2 and r.connections[(0, 1)].trade_gaps == 0
    assert r.connections[(0, 2)].depth_gaps == 0
    assert r.snapshots == [106] and r.snapshot_syncable(106)
    assert report_ok(r)


def test_inspect_detects_gaps_and_unsyncable_snapshot(tmp_path):
    _write(tmp_path, [
        _depth(1, T0, 100, 105),
        _depth(1, T0 + 1, 107, 110),  # 106 missing
        _trade(1, T0 + 2, 7),
        _trade(1, T0 + 3, 9),  # 8 missing
        make_envelope(T0 + 4, "snapshot", json.dumps({"lastUpdateId": 200, "bids": [], "asks": []}), t0=T0),
    ])
    r = inspect_symbol(tmp_path, "BTCUSDT")
    assert r.connections[(0, 1)].depth_gaps == 1
    assert r.connections[(0, 1)].trade_gaps == 1
    assert not r.snapshot_syncable(200)
    assert not report_ok(r)


def test_inspect_handover_between_connections(tmp_path):
    _write(tmp_path, [
        _depth(1, T0, 100, 105),
        _depth(1, T0 + 1, 106, 110),
        make_envelope(T0 + 2, "snapshot", json.dumps({"lastUpdateId": 104, "bids": [], "asks": []}), t0=T0),
        _depth(2, T0 + 3, 108, 112),  # overlaps conn 1: seamless
        _depth(3, T0 + 4, 120, 125),  # conn 2 ended at 112: 113..119 missed
    ])
    r = inspect_symbol(tmp_path, "BTCUSDT")
    assert r.handover_gaps() == [(2, 3, 7)]
    assert not report_ok(r)


def _connecting(conn, t):
    return make_envelope(t, "event", {"event": "connecting"}, conn=conn)


def test_inspect_separates_recorder_runs(tmp_path):
    _write(tmp_path, [
        _connecting(1, T0),
        _depth(1, T0 + 1, 100, 105),
        make_envelope(T0 + 2, "snapshot", json.dumps({"lastUpdateId": 104, "bids": [], "asks": []}), t0=T0),
        _depth(1, T0 + 3, 106, 110),
        # recorder restarted: connection IDs start again at 1, and updates 111..149 were never received
        _connecting(1, T0 + 10),
        _depth(1, T0 + 11, 150, 155),
        make_envelope(T0 + 12, "snapshot", json.dumps({"lastUpdateId": 152, "bids": [], "asks": []}), t0=T0),
        _depth(1, T0 + 13, 156, 160),
    ])
    r = inspect_symbol(tmp_path, "BTCUSDT")
    assert set(r.connections) == {(0, 1), (1, 1)}
    assert all(c.depth_gaps == 0 for c in r.connections.values())
    assert r.restart_gaps() == [(0, 1, 39)]
    assert r.handover_gaps() == []
    assert report_ok(r)  # a restart gap is expected, not an archive defect
