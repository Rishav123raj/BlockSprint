import sys

import pytest

from research.data import keep_awake


def test_unsupported_platform_is_a_no_op(monkeypatch):
    monkeypatch.setattr(keep_awake.sys, "platform", "linux")
    calls = []
    monkeypatch.setattr(keep_awake, "_set_state", lambda flags: calls.append(flags) or 1)
    with keep_awake.KeepAwake() as awake:
        assert not awake.active
    assert calls == []


def test_flags_requested_and_released(monkeypatch):
    monkeypatch.setattr(keep_awake.sys, "platform", "win32")
    calls = []
    monkeypatch.setattr(keep_awake, "_set_state", lambda flags: calls.append(flags) or 0x80000000)
    with keep_awake.KeepAwake() as awake:
        assert awake.active
    assert calls == [keep_awake.ES_CONTINUOUS | keep_awake.ES_SYSTEM_REQUIRED, keep_awake.ES_CONTINUOUS]


def test_refused_request_is_not_released(monkeypatch):
    monkeypatch.setattr(keep_awake.sys, "platform", "win32")
    calls = []
    monkeypatch.setattr(keep_awake, "_set_state", lambda flags: calls.append(flags) or 0)
    with keep_awake.KeepAwake() as awake:
        assert not awake.active
    assert calls == [keep_awake.ES_CONTINUOUS | keep_awake.ES_SYSTEM_REQUIRED]


@pytest.mark.skipif(sys.platform != "win32", reason="Windows only")
def test_real_windows_call_is_accepted():
    with keep_awake.KeepAwake() as awake:
        assert awake.active
