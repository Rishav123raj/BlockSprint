"""Order latency on the Binance Spot Testnet (P7.7, docs/calibration.md): a second reference for the latency presets.

The testnet's liquidity isn't representative, so this measures latency only, never fills:

* the REST round trip (``GET /api/v3/ping``) and the clock offset to the server (``GET /api/v3/time``);
* with testnet keys: ``--orders`` post-only (``LIMIT_MAKER``) buys of the minimum size at 20% below the best bid,
  each cancelled at once. It times the place round trip, the cancel round trip, and the entry leg one way
  (the server's ``transactTime`` against the local send time, corrected by the clock offset).

Keys come only from ``BINANCE_TESTNET_API_KEY`` and ``BINANCE_TESTNET_API_SECRET`` (create them at
https://testnet.binance.vision); this never reads the mainnet variables and only talks to the testnet host. Without
keys it measures the public part.

Usage::

    uv run python -m research.live.testnet --symbol BTCUSDT --n 60 --orders 30 --out data/testnet/2026-11-14.json
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import time
from decimal import ROUND_DOWN, ROUND_UP, Decimal
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import numpy as np
import requests

BASE = "https://testnet.binance.vision"


def sign(params: dict[str, Any], secret: str) -> str:
    """The query string with its HMAC-SHA256 ``signature`` appended (Binance's SIGNED endpoints)."""
    query = urlencode(params)
    sig = hmac.new(secret.encode(), query.encode(), hashlib.sha256).hexdigest()
    return f"{query}&signature={sig}"


def quantiles(ms: list[float]) -> dict[str, float] | None:
    if not ms:
        return None
    a = np.asarray(ms)
    return {"n": len(ms), "mean": round(float(a.mean()), 3),
            **{f"p{q}": round(float(np.percentile(a, q)), 3) for q in (10, 50, 90, 99)}, "max": round(float(a.max()), 3)}


def suggested_leg(rtt: dict[str, float]) -> dict[str, Any]:
    """A one-way leg for the simulator's latency config: lognormal with half the round trip's median, sigma from its
    p50..p90 spread (p90 of a lognormal is exp(1.2816 sigma) times the median), min half its p10."""
    sigma = max(0.05, float(np.log(rtt["p90"] / rtt["p50"]) / 1.2816)) if rtt["p50"] > 0 else 0.3
    return {"model": "lognormal", "median_us": round(rtt["p50"] * 500), "sigma": round(sigma, 2),
            "min_us": round(rtt["p10"] * 500)}


def _round(x: Decimal, step: Decimal, mode: str) -> Decimal:
    return (x / step).quantize(Decimal(1), rounding=mode) * step


class TestnetClient:
    """Signs requests for the testnet only: the host is fixed (CI checks it), never configurable."""
    __test__ = False  # not a pytest class, despite the name

    def __init__(self, key: str | None = None, secret: str | None = None, session: Any = None):
        self.key, self.secret, self.base = key, secret, BASE
        self.http = session or requests.Session()
        self.offset_ms = 0.0  # server clock minus local clock

    def _get(self, path: str, **params: Any) -> Any:
        r = self.http.get(self.base + path, params=params or None, timeout=10)
        r.raise_for_status()
        return r.json()

    def _signed(self, method: str, path: str, params: dict[str, Any]) -> Any:
        if self.base != BASE:
            raise RuntimeError(f"refusing to sign a request for {self.base}: testnet only")
        params = {**params, "recvWindow": 5000, "timestamp": int(time.time() * 1000 + self.offset_ms)}
        r = self.http.request(method, f"{self.base}{path}?{sign(params, self.secret)}",
                              headers={"X-MBX-APIKEY": self.key}, timeout=10)
        r.raise_for_status()
        return r.json()

    def ping(self, n: int, pause_s: float = 0.2) -> list[float]:
        self._get("/api/v3/ping")  # opens the kept-alive connection; not counted
        out = []
        for _ in range(n):
            t = time.perf_counter()
            self._get("/api/v3/ping")
            out.append((time.perf_counter() - t) * 1000)
            time.sleep(pause_s)
        return out

    def clock(self, n: int = 10) -> float:
        """The server's clock minus ours, in ms, from the time request with the shortest round trip."""
        best = None
        for _ in range(n):
            t0 = time.time() * 1000
            server = self._get("/api/v3/time")["serverTime"]
            t1 = time.time() * 1000
            if best is None or t1 - t0 < best[0]:
                best = (t1 - t0, server - (t0 + t1) / 2)
        self.offset_ms = best[1]
        return self.offset_ms

    def orders(self, symbol: str, n: int, pause_s: float = 0.5) -> dict[str, list[float]]:
        """n post-only minimum-size buys far below the market, each cancelled at once."""
        info = self._get("/api/v3/exchangeInfo", symbol=symbol)["symbols"][0]
        f = {x["filterType"]: x for x in info["filters"]}
        tick, step = Decimal(f["PRICE_FILTER"]["tickSize"]), Decimal(f["LOT_SIZE"]["stepSize"])
        min_notional = Decimal((f.get("NOTIONAL") or f.get("MIN_NOTIONAL") or {}).get("minNotional", "0"))
        place, cancel, entry = [], [], []
        for _ in range(n):
            bid = Decimal(self._get("/api/v3/ticker/bookTicker", symbol=symbol)["bidPrice"])
            price = _round(bid * Decimal("0.8"), tick, ROUND_DOWN)
            qty = max(Decimal(f["LOT_SIZE"]["minQty"]), _round(min_notional * Decimal("1.1") / price, step, ROUND_UP))
            sent = time.time() * 1000
            t = time.perf_counter()
            o = self._signed("POST", "/api/v3/order", {"symbol": symbol, "side": "BUY", "type": "LIMIT_MAKER",
                                                       "quantity": format(qty.normalize(), "f"),
                                                       "price": format(price.normalize(), "f"),
                                                       "newOrderRespType": "ACK"})
            place.append((time.perf_counter() - t) * 1000)
            entry.append(o["transactTime"] - (sent + self.offset_ms))
            t = time.perf_counter()
            self._signed("DELETE", "/api/v3/order", {"symbol": symbol, "orderId": o["orderId"]})
            cancel.append((time.perf_counter() - t) * 1000)
            time.sleep(pause_s)
        return {"place_rtt_ms": place, "cancel_rtt_ms": cancel, "entry_one_way_ms": entry}


def measure(symbol: str, n: int, orders: int, client: TestnetClient | None = None) -> dict[str, Any]:
    client = client or TestnetClient(os.environ.get("BINANCE_TESTNET_API_KEY"), os.environ.get("BINANCE_TESTNET_API_SECRET"))
    out: dict[str, Any] = {"host": client.base, "symbol": symbol,
                           "time_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    out["ping_rtt_ms"] = quantiles(client.ping(n))
    out["clock_offset_ms"] = round(client.clock(), 3)
    if client.key and client.secret and orders > 0:
        o = client.orders(symbol, orders)
        out.update({k: quantiles(v) for k, v in o.items()})
        out["suggested_leg"] = suggested_leg(out["place_rtt_ms"])
    else:
        out["orders"] = "skipped: set BINANCE_TESTNET_API_KEY and BINANCE_TESTNET_API_SECRET"
        out["suggested_leg"] = suggested_leg(out["ping_rtt_ms"])
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--symbol", default="BTCUSDT")
    p.add_argument("--n", type=int, default=60, help="pings")
    p.add_argument("--orders", type=int, default=30, help="place-and-cancel pairs (needs testnet keys)")
    p.add_argument("--out", help="write the result here (JSON)")
    args = p.parse_args(argv)
    result = measure(args.symbol.upper(), args.n, args.orders)
    text = json.dumps(result, indent=2)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
