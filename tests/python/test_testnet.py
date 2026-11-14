"""The testnet latency tool (P7.7) against a fake HTTP session: signing, the orders it sends, the measures."""

from urllib.parse import parse_qs, urlparse

import pytest

from research.live.testnet import BASE, TestnetClient, measure, sign, suggested_leg


def test_signature_matches_binances_documented_example():
    secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"
    params = {"symbol": "LTCBTC", "side": "BUY", "type": "LIMIT", "timeInForce": "GTC", "quantity": 1,
              "price": 0.1, "recvWindow": 5000, "timestamp": 1499827319559}
    assert sign(params, secret).endswith("&signature=c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71")


class Resp:
    def __init__(self, body):
        self.body = body

    def raise_for_status(self):
        pass

    def json(self):
        return self.body


class FakeExchange:
    """Answers the testnet's endpoints; records the signed requests."""

    def __init__(self):
        self.signed = []
        self.next_id = 1

    def get(self, url, params=None, timeout=None):
        path = url[len(BASE):]
        if path == "/api/v3/ping":
            return Resp({})
        if path == "/api/v3/time":
            return Resp({"serverTime": 0})
        if path == "/api/v3/exchangeInfo":
            return Resp({"symbols": [{"filters": [
                {"filterType": "PRICE_FILTER", "tickSize": "0.01000000"},
                {"filterType": "LOT_SIZE", "minQty": "0.00001000", "stepSize": "0.00001000"},
                {"filterType": "NOTIONAL", "minNotional": "5.00000000"}]}]})
        if path == "/api/v3/ticker/bookTicker":
            return Resp({"bidPrice": "70000.01", "askPrice": "70000.02"})
        raise AssertionError(path)

    def request(self, method, url, headers=None, timeout=None):
        u = urlparse(url)
        assert u.scheme + "://" + u.netloc == BASE and headers["X-MBX-APIKEY"] == "k"
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        self.signed.append((method, u.path, q))
        if method == "POST":
            self.next_id += 1
            return Resp({"orderId": self.next_id, "transactTime": 0})
        return Resp({})


def test_orders_are_post_only_minimum_buys_far_below_the_market_and_cancelled(monkeypatch):
    monkeypatch.setattr("research.live.testnet.time.sleep", lambda s: None)
    fake = FakeExchange()
    r = measure("BTCUSDT", n=3, orders=2, client=TestnetClient("k", "s", fake))
    assert [m for m, _, _ in fake.signed] == ["POST", "DELETE", "POST", "DELETE"]
    _, path, q = fake.signed[0]
    assert path == "/api/v3/order" and q["type"] == "LIMIT_MAKER" and q["side"] == "BUY"
    assert q["price"] == "56000" and q["quantity"] == "0.0001"   # 80% of the bid; 1.1 x 5 USDT / price, rounded up
    assert fake.signed[1][2]["orderId"] == "2" and "signature" in q
    assert r["place_rtt_ms"]["n"] == 2 and r["cancel_rtt_ms"]["n"] == 2 and r["ping_rtt_ms"]["n"] == 3
    assert r["suggested_leg"]["model"] == "lognormal"


def test_without_keys_only_the_public_part_runs(monkeypatch):
    monkeypatch.setattr("research.live.testnet.time.sleep", lambda s: None)
    fake = FakeExchange()
    r = measure("BTCUSDT", n=2, orders=5, client=TestnetClient(None, None, fake))
    assert not fake.signed and r["orders"].startswith("skipped") and r["ping_rtt_ms"]["n"] == 2


def test_suggested_leg_is_half_the_round_trip():
    leg = suggested_leg({"p10": 170.0, "p50": 175.0, "p90": 189.0})
    assert leg["median_us"] == 87500 and leg["min_us"] == 85000
    assert leg["sigma"] == pytest.approx(0.06, abs=0.01)
