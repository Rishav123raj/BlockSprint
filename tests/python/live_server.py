"""Shared by the live-mode tests: the server binary on a free port (P7.1, P7.2), skipped when it isn't built."""

import json
import socket
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
_CANDIDATES = [ROOT / "build" / "Release" / "crypto_matching_engine.exe", ROOT / "build" / "crypto_matching_engine",
               ROOT / "build" / "crypto_matching_engine.exe"]
SERVER = next((p for p in _CANDIDATES if p.exists()), None)
needs_server = pytest.mark.skipif(SERVER is None, reason="the server isn't built (cmake --build build)")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def server():
    """The port of a fresh server (cancel_from_back queue model), stopped after the module."""
    if SERVER is None:
        pytest.skip("the server isn't built")
    port = _free_port()
    proc = subprocess.Popen([str(SERVER), "--port", str(port), "--no-log", "--feed-queue-model", "cancel_from_back"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/api/market-feed", timeout=0.2)
            break
        except OSError:
            time.sleep(0.05)
    else:
        proc.kill()
        pytest.fail("the server didn't start")
    yield port
    proc.terminate()
    proc.wait(timeout=5)


def http(port, method, path, body=None):
    """A JSON request to the server; an HTTP error's JSON body is returned too."""
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method,
                                 data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        return json.loads(e.read())
