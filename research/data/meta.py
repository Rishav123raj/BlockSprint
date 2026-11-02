"""Venue metadata and local clock quality samples recorded alongside market data."""

from __future__ import annotations

import json
import socket
import struct
import time
from typing import Any

import requests

BINANCE_REST = "https://api.binance.com"

# Seconds between the NTP epoch (1900) and the Unix epoch (1970)
_NTP_EPOCH_OFFSET = 2208988800


def fetch_exchange_info(symbols: list[str], timeout: float = 10.0) -> str:
    """Raw ``/api/v3/exchangeInfo`` text for the given symbols (tick size, lot size, notional filters)."""
    response = requests.get(
        f"{BINANCE_REST}/api/v3/exchangeInfo",
        params={"symbols": json.dumps(symbols, separators=(",", ":"))},
        timeout=timeout,
    )
    response.raise_for_status()
    return response.text


def trading_symbols(exchange_info_text: str) -> dict[str, str]:
    """Map of symbol -> status from an exchangeInfo payload."""
    return {s["symbol"]: s["status"] for s in json.loads(exchange_info_text)["symbols"]}


def sntp_sample(server: str = "pool.ntp.org", timeout: float = 2.0) -> dict[str, Any]:
    """One SNTP measurement of the local clock against ``server``.

    Returns ``offset_ns`` (server time minus local time; add it to local timestamps to correct them)
    and ``delay_ns`` (network round trip), following RFC 4330.
    """
    packet = b"\x23" + 47 * b"\0"  # LI=0, version 4, mode 3 (client)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        t1 = time.time_ns()
        sock.sendto(packet, (server, 123))
        data, _ = sock.recvfrom(512)
        t4 = time.time_ns()
    if len(data) < 48:
        raise ValueError(f"short NTP reply ({len(data)} bytes)")

    def ntp_to_ns(seconds: int, fraction: int) -> int:
        return (seconds - _NTP_EPOCH_OFFSET) * 1_000_000_000 + (fraction * 1_000_000_000 >> 32)

    receive_s, receive_f, transmit_s, transmit_f = struct.unpack("!IIII", data[32:48])
    t2 = ntp_to_ns(receive_s, receive_f)
    t3 = ntp_to_ns(transmit_s, transmit_f)
    return {
        "server": server,
        "offset_ns": ((t2 - t1) + (t3 - t4)) // 2,
        "delay_ns": (t4 - t1) - (t3 - t2),
    }
