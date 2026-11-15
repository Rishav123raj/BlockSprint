"""Binance market data into the BlockSprint engine.

Runs connector v2 (P7.2): research/live/feed_bridge.py streams every depth update and trade to /ws/market-feed,
where the simulator's Synthesizer builds the book (queue positions, trades reaching resting orders).
The 1.0 connector, which mirrored the top-10 partial book as one MARKET order per level, was removed in P7.8
after v2's soak test (docs/baseline/soak_2026-11-14.md).

    uv run scripts/binance_connector.py
"""
import argparse
import asyncio
import logging
import os
import sys
import requests

# Only public Binance market-data endpoints are used, so no API key is required.

def get_active_symbols(limit=10):
    """
    Active Symbol Discovery & Filtering
    Queries public exchangeInfo and filters for major trading pairs.
    """
    print("Discovering active Binance symbols...")
    url = "https://api.binance.com/api/v3/exchangeInfo"
    response = requests.get(url)
    response.raise_for_status()
    data = response.json()

    symbols = []
    for s in data["symbols"]:
        if s["status"] == "TRADING" and s["quoteAsset"] == "USDT":
            symbols.append(s["symbol"])

    # Keep top high-liquidity symbols to avoid rate limits
    major_symbols = ["BTCUSDT", "ETHUSDT", "SOLUSDT", "ADAUSDT", "DOGEUSDT", "XRPUSDT", "LTCUSDT"]
    filtered_symbols = [s for s in symbols if s in major_symbols][:limit]

    print(f"Selected symbols: {filtered_symbols}")
    return filtered_symbols

def run_v2(symbols):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from research.live.feed_bridge import FeedBridge

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    bridge = FeedBridge(symbols)
    asyncio.run(bridge.run())


if __name__ == "__main__":
    # No options; an old command line with --v1 fails here instead of silently running v2
    argparse.ArgumentParser(description="Binance market data into the BlockSprint engine").parse_args()
    symbols = get_active_symbols(limit=5)
    try:
        run_v2(symbols)
    except KeyboardInterrupt:
        print("\nConnector stopped. Exiting.")
