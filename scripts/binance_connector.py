"""Binance market data into the BlockSprint engine.

v2 (the default, P7.2): research/live/feed_bridge.py streams every depth update and trade to /ws/market-feed,
where the simulator's Synthesizer builds the book (queue positions, trades reaching resting orders).
v1 (--v1, kept until v2 has run stably for two weeks, P7.8): mirrors the top-10 partial book as one MARKET order
per level, re-submitting a level whenever it changes.

    uv run scripts/binance_connector.py              # v2, the same 5 symbols as v1
    uv run scripts/binance_connector.py --v1
"""
import argparse
import asyncio
import logging
import os
import sys
import websockets
import json
import requests

# Only public Binance market-data endpoints are used, so no API key is required.

# BlockSprint REST API configuration
ENGINE_REST_URL = "http://127.0.0.1:8080/api/orders"
ENGINE_WS_URL = "ws://127.0.0.1:8080/ws/orders"

# Keep track of active price levels we have populated
# Structure: {(symbol, side, price): (order_id, quantity)}
active_orders = {}

# Mirrored liquidity is submitted as owner MARKET with STP policy "skip": mirrored levels never trade
# with each other, but user orders trade with them. The engine accepts this owner only from localhost.
MARKET_OWNER = "MARKET"

def reset_engine():
    """Call C++ REST endpoint /api/reset to ensure clean state before starting connector."""
    try:
        url = "http://127.0.0.1:8080/api/reset"
        res = requests.post(url, timeout=2)
        print("Matching engine state reset successfully before connector start.")
    except Exception as e:
        print(f"Could not reset matching engine: {e}")

def register_symbol_specs(symbols):
    """Load Binance's trading rules (tick size, lot size, quantity range, min notional) for the mirrored
    symbols into the engine, so user orders are validated against the exchange's real filters.
    Mirrored levels themselves are exempt (they are aggregates, not orders)."""
    try:
        res = requests.get(
            "https://api.binance.com/api/v3/exchangeInfo",
            params={"symbols": json.dumps(symbols, separators=(",", ":"))},
            timeout=10,
        )
        res.raise_for_status()
        for info in res.json()["symbols"]:
            filters = {f["filterType"]: f for f in info["filters"]}
            price = filters.get("PRICE_FILTER", {})
            lot = filters.get("LOT_SIZE", {})
            notional = filters.get("NOTIONAL") or filters.get("MIN_NOTIONAL") or {}
            spec = {
                "symbol": info["symbol"].replace("USDT", "-USDT"),
                "tick_size": price.get("tickSize", "0"),
                "lot_size": lot.get("stepSize", "0"),
                "min_qty": lot.get("minQty", "0"),
                "max_qty": lot.get("maxQty", "0"),
                "min_notional": notional.get("minNotional", "0"),
            }
            requests.post("http://127.0.0.1:8080/api/symbols", json=spec, timeout=3).raise_for_status()
            print(f"Trading rules for {spec['symbol']}: tick {spec['tick_size']}, lot {spec['lot_size']}, "
                  f"min notional {spec['min_notional']}")
    except Exception as e:
        print(f"Could not load Binance trading rules (orders will only get basic validation): {e}")

def get_order_id(symbol, side, price):
    """Generate a consistent, unique order ID for a Binance price level."""
    return f"BINANCE_{symbol.replace('-', '_')}_{side.upper()}_{float(price):.6f}"

async def submit_external_order(engine_ws, symbol, side, price, qty):
    """Mirror a Binance price level as a resting limit order in the C++ engine.

    Levels whose quantity is unchanged are left alone, so they keep their queue position and any
    liquidity a user order consumed stays consumed until Binance's quantity changes. A changed level
    is replaced (back of the queue).
    """
    order_key = (symbol, side.lower(), float(price))
    current = active_orders.get(order_key)
    if current is not None and current[1] == float(qty):
        return
    order_id = get_order_id(symbol, side, price)
    payload = {
        "action": "replace",
        "order_id": order_id,
        "symbol": symbol,
        "order_type": "limit",
        "side": side.lower(),
        "price": str(price),
        "quantity": str(qty),
        "owner": MARKET_OWNER,
        "stp": "skip"
    }
    await engine_ws.send(json.dumps(payload))
    active_orders[order_key] = (order_id, float(qty))

async def cancel_external_order(engine_ws, symbol, side, price):
    """Cancel a resting order in the C++ engine over WebSockets."""
    order_key = (symbol, side.lower(), float(price))
    if order_key in active_orders:
        order_id = active_orders[order_key][0]
        payload = {
            "action": "cancel",
            "symbol": symbol,
            "order_id": order_id
        }
        await engine_ws.send(json.dumps(payload))
        del active_orders[order_key]

async def drain_responses(engine_ws):
    """Continuously read and discard confirmations from the BlockSprint server to prevent TCP buffer bloat."""
    try:
        async for _ in engine_ws:
            pass
    except asyncio.CancelledError:
        pass
    except Exception as e:
        print(f"Drain task stopped: {e}")

def get_active_symbols(limit=10):
    """
    1. Active Symbol Discovery & Filtering
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

async def load_rest_snapshots(engine_ws, symbols):
    """
    2. Loading the REST Snapshot
    Populates initial order book state for each symbol.
    """
    print("\n--- Bootstrapping Order Books with REST Snapshots ---")
    url = "https://api.binance.com/api/v3/depth"
    
    for symbol in symbols:
        formatted_symbol = symbol.replace("USDT", "-USDT")
        print(f"Loading snapshot for {formatted_symbol}...")
        
        params = {"symbol": symbol, "limit": "5"}
        # Fetch in a worker thread: a blocking call here would freeze the event loop, and orders already
        # passed to engine_ws.send() would sit unsent in its buffer, leaving the previous symbol's book
        # half-built in the engine for as long as Binance takes to answer
        res = await asyncio.to_thread(requests.get, url, params=params, timeout=10)
        res.raise_for_status()
        depth = res.json()
        
        # Populate Bids
        for bid in depth["bids"]:
            price, qty = bid[0], bid[1]
            await submit_external_order(engine_ws, formatted_symbol, "buy", price, qty)
            
        # Populate Asks
        for ask in depth["asks"]:
            price, qty = ask[0], ask[1]
            await submit_external_order(engine_ws, formatted_symbol, "sell", price, qty)
            
    print("REST bootstrapping complete.\n")

async def sync_websocket_deltas(active_symbols):
    """
    3. Real-Time WebSocket Synchronization
    Listens to live Binance streams and synchronizes mutations.
    Includes connection recovery logic to handle C++ server restarts.
    """
    streams = "/".join([f"{s.lower()}@depth10@100ms" for s in active_symbols])
    binance_ws_url = f"wss://stream.binance.com:9443/stream?streams={streams}"
    
    while True:
        try:
            # 1. Reset C++ engine to clean start before bootstrap snapshot
            reset_engine()
            register_symbol_specs(active_symbols)
            
            print(f"Connecting to Binance WebSocket: {binance_ws_url}")
            print(f"Connecting to BlockSprint WebSocket: {ENGINE_WS_URL}")
            
            async with websockets.connect(binance_ws_url) as binance_ws, \
                       websockets.connect(ENGINE_WS_URL) as engine_ws:
                       
                # Clear local active orders state when starting a new session
                active_orders.clear()
                
                # Start a background task to drain messages from engine_ws
                drain_task = asyncio.create_task(drain_responses(engine_ws))
                
                try:
                    # Bootstrap book snapshots first
                    await load_rest_snapshots(engine_ws, active_symbols)
                    
                    print("Listening for real-time WebSocket book deltas...")
                    while True:
                        frame = await binance_ws.recv()
                        msg = json.loads(frame)
                        
                        stream = msg["stream"]
                        data = msg["data"]
                        
                        # Format: "ETHUSDT" from stream name
                        raw_symbol = stream.split("@")[0].upper()
                        symbol = raw_symbol.replace("USDT", "-USDT")
                        
                        # 1. Remove levels that left the snapshot, on BOTH sides, before adding anything.
                        #    Every level left afterwards is part of the new (uncrossed) Binance snapshot,
                        #    so no update below can cross the book. Applying all bids before removing
                        #    stale asks is what produced crossed books in 1.0 (D7).
                        new_prices = {
                            "buy": {float(bid[0]) for bid in data["bids"]},
                            "sell": {float(ask[0]) for ask in data["asks"]},
                        }
                        stale = [
                            (side, price) for (sym, side, price) in list(active_orders.keys())
                            if sym == symbol and price not in new_prices[side]
                        ]
                        for side, price in stale:
                            await cancel_external_order(engine_ws, symbol, side, price)

                        # 2. Add new levels and replace levels whose quantity changed
                        for side, levels in (("buy", data["bids"]), ("sell", data["asks"])):
                            for level in levels:
                                price, qty = float(level[0]), float(level[1])
                                if qty == 0.0:
                                    await cancel_external_order(engine_ws, symbol, side, price)
                                else:
                                    await submit_external_order(engine_ws, symbol, side, price, qty)
                                
                finally:
                    drain_task.cancel()
                    await asyncio.gather(drain_task, return_exceptions=True)
                    
        except Exception as e:
            print(f"Connection lost or handshake failed: {e}. Retrying in 3 seconds...")
            await asyncio.sleep(3)

def run_v2(symbols):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from research.live.feed_bridge import FeedBridge

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    bridge = FeedBridge(symbols)
    asyncio.run(bridge.run())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Binance market data into the BlockSprint engine")
    parser.add_argument("--v1", action="store_true", help="the 1.0 connector (one MARKET order per top-10 level)")
    args = parser.parse_args()
    symbols = get_active_symbols(limit=5)
    try:
        if args.v1:
            reset_engine()
            asyncio.run(sync_websocket_deltas(symbols))
        else:
            run_v2(symbols)
    except KeyboardInterrupt:
        print("\nConnector stopped. Exiting.")
