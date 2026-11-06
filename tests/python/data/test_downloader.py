"""Public data downloader (P2.11): checksum verification and CSV conversion, offline."""

import hashlib
import zipfile

import numpy as np
import pytest

from research.data.downloader import (
    DownloadError, fetch, klines_table, parse_checksum, read_csv_rows, to_ns, trades_table, url_for,
)
from research.data.schema import AGGRESSOR_BUY, AGGRESSOR_SELL, KIND_TRADE, Spec

SPEC = Spec("BTCUSDT", tick_size=1_000_000, lot_size=1_000)  # 0.01 / 0.00001
URL = "https://data.binance.vision/data/spot/daily/trades/BTCUSDT/BTCUSDT-trades-2026-11-04.zip"


def make_zip(path, rows):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(path.name.replace(".zip", ".csv"), "\n".join(",".join(r) for r in rows) + "\n")


def cached(tmp_path, rows, checksum=None):
    cache = tmp_path / "cache"
    cache.mkdir(parents=True)
    z = cache / "BTCUSDT-trades-2026-11-04.zip"
    make_zip(z, rows)
    digest = checksum or hashlib.sha256(z.read_bytes()).hexdigest()
    (cache / (z.name + ".CHECKSUM")).write_text(f"{digest}  {z.name}\n")
    return cache, z


def test_urls():
    assert url_for("trades", "BTCUSDT", "2026-11-04") == URL
    assert url_for("klines", "BTCUSDT", "2026-11-04").endswith("/klines/BTCUSDT/1m/BTCUSDT-1m-2026-11-04.zip")


def test_checksum_is_verified_and_a_bad_download_is_deleted(tmp_path):
    rows = [["1", "77000.01000000", "0.00100000", "77.00001000", "1793750400000000", "True", "True"]]
    cache, z = cached(tmp_path, rows)
    assert fetch(URL, cache) == z  # cached files are re-verified, not re-downloaded
    bad_cache, bad = cached(tmp_path / "b", rows, checksum="0" * 64)
    with pytest.raises(DownloadError, match="SHA-256 mismatch"):
        fetch(URL, bad_cache)
    assert not bad.exists()
    with pytest.raises(DownloadError, match="malformed"):
        parse_checksum("not a checksum")


def test_timestamps_in_milliseconds_or_microseconds():
    assert to_ns(np.array([1730000000000])).tolist() == [1730000000000 * 10**6]     # ms (before 2025)
    assert to_ns(np.array([1793750400000000])).tolist() == [1793750400000000 * 10**3]  # µs (2025 onwards)


def test_trades_and_agg_trades_convert_to_the_normalized_schema(tmp_path):
    rows = [["7", "77000.01000000", "0.00150000", "115.50001500", "1793750400123456", "True", "True"],
            ["8", "77000.02000000", "0.00001000", "0.77000020", "1793750400123999", "False", "True"]]
    _, z = cached(tmp_path, rows)
    table, off = trades_table(read_csv_rows(z), SPEC, aggregated=False)
    t = table.to_pydict()
    assert off == 0
    assert t["seq"] == [7, 8] and t["px"] == [7700001, 7700002] and t["qty"] == [150, 1]
    assert t["side"] == [AGGRESSOR_SELL, AGGRESSOR_BUY] and set(t["kind"]) == {KIND_TRADE}
    assert t["ts_local"] == t["ts_exch"] == [1793750400123456000, 1793750400123999000]

    agg = [["100", "77000.01000000", "0.00150000", "7", "9", "1793750400123456", "False", "True"]]
    table, _ = trades_table(agg, SPEC, aggregated=True)
    a = table.to_pydict()
    assert (a["seq"], a["seq_first"], a["side"]) == ([9], [7], [AGGRESSOR_BUY])


def test_klines_convert(tmp_path):
    rows = [["1793750400000000", "77000.00000000", "77010.00000000", "76990.00000000", "77005.00000000",
             "1.50000000", "1793750459999999", "115500.00000000", "42", "0.75000000", "57750.00000000", "0"]]
    k = klines_table(rows, SPEC).to_pydict()
    assert k["open"] == [7700000] and k["high"] == [7701000] and k["volume"] == [150000]
    assert k["trades"] == [42] and k["quote_volume"] == [115500 * 10**8]
    assert k["close_time"] == [1793750459999999000]
