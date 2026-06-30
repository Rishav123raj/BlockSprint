import asyncio
import websockets
import json
import requests
import os
from dotenv import load_dotenv

# Load API credentials from .env file
load_dotenv()
BINANCE_API_KEY = os.getenv("BINANCE_API_KEY")
BINANCE_API_SECRET = os.getenv("BINANCE_API_SECRET")

# BlockSprint REST API configuration
ENGINE_REST_URL = "http://127.0.0.1:8080/api/orders"
ENGINE_WS_URL = "ws://127.0.0.1:8080/ws/orders"

# Keep track of active price levels we have populated
# Structure: {(symbol, side, price): order_id}
active_orders = {}

def get_order_id(symbol, side, price):
    """Generate a consistent, unique order ID for a Binance price level."""
    return f"BINANCE_{symbol.replace('-', '_')}_{side.upper()}_{float(price):.6f}"

async def submit_external_order(engine_ws, symbol, side, price, qty):
    """Submit a resting limit order to the C++ engine over WebSockets."""
    order_id = get_order_id(symbol, side, price)
    payload = {
        "action": "submit",
        "order_id": order_id,
        "symbol": symbol,
        "order_type": "limit",
        "side": side.lower(),
        "price": str(price),
        "quantity": str(qty)
    }
    await engine_ws.send(json.dumps(payload))
    active_orders[(symbol, side.lower(), float(price))] = order_id

async def cancel_external_order(engine_ws, symbol, side, price):
    """Cancel a resting order in the C++ engine over WebSockets."""
    order_key = (symbol, side.lower(), float(price))
    if order_key in active_orders:
        order_id = active_orders[order_key]
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
        res = requests.get(url, params=params)
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
    """
    # Stream multiplex format: wss://stream.binance.com:9443/stream?streams=ethusdt@depth10@100ms/...
    streams = "/".join([f"{s.lower()}@depth10@100ms" for s in active_symbols])
    binance_ws_url = f"wss://stream.binance.com:9443/stream?streams={streams}"
    
    print(f"Connecting to Binance WebSocket: {binance_ws_url}")
    print(f"Connecting to BlockSprint WebSocket: {ENGINE_WS_URL}")
    
    async with websockets.connect(binance_ws_url) as binance_ws, \
               websockets.connect(ENGINE_WS_URL) as engine_ws:
               
        # Start a background task to drain messages from engine_ws
        drain_task = asyncio.create_task(drain_responses(engine_ws))
        
        try:
            # Bootstrap book snapshots first
            await load_rest_snapshots(engine_ws, active_symbols)
            
            print("Listening for real-time WebSocket book deltas...")
            while True:
                try:
                    frame = await binance_ws.recv()
                    msg = json.loads(frame)
                    
                    stream = msg["stream"]
                    data = msg["data"]
                    
                    # Format: "ETHUSDT" from stream name
                    raw_symbol = stream.split("@")[0].upper()
                    symbol = raw_symbol.replace("USDT", "-USDT")
                    
                    # Sync Bids
                    for bid in data["bids"]:
                        price, qty = float(bid[0]), float(bid[1])
                        if qty == 0.0:
                            await cancel_external_order(engine_ws, symbol, "buy", price)
                        else:
                            await submit_external_order(engine_ws, symbol, "buy", price, qty)
                            
                    # Sync Asks
                    for ask in data["asks"]:
                        price, qty = float(ask[0]), float(ask[1])
                        if qty == 0.0:
                            await cancel_external_order(engine_ws, symbol, "sell", price)
                        else:
                            await submit_external_order(engine_ws, symbol, "sell", price, qty)
                            
                except Exception as e:
                    print(f"Sync error: {e}")
                    await asyncio.sleep(1)
        finally:
            drain_task.cancel()
            await asyncio.gather(drain_task, return_exceptions=True)

if __name__ == "__main__":
    symbols = get_active_symbols(limit=5)
    try:
        asyncio.run(sync_websocket_deltas(symbols))
    except KeyboardInterrupt:
        print("\nConnector stopped. Exiting.")
