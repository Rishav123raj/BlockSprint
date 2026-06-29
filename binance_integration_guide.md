# BlockSprint - Binance Live Order Book Integration Guide

This guide details the implementation path to connect **BlockSprint** to the live Binance Order Book API. It shows how to discover symbols, initialize the book using REST snapshots, sync real-time book updates using WebSockets, and modify the C++ engine to handle external liquidity.

---

## 1. Architectural Strategy: The "Snapshot + Delta" Pattern

Using pure REST polling is too slow and will quickly trigger rate limits. Instead, we use the industry-standard **Snapshot + WebSocket Delta** pattern:

1. **Startup Discovery**: Query the Binance REST API for all active ticker symbols, filter them down to a configured list of active high-volume trading pairs (e.g. USDT markets).
2. **Initial Bootstrap**: Pull the L2 order book snapshot for each symbol *once* via the REST `/api/v3/depth` endpoint. Load this snapshot into BlockSprint as the initial resting order book state.
3. **Real-Time Sync**: Open a multiplexed WebSocket stream subscribing to the `@depth10@100ms` feed for all active symbols. Forward these frames to BlockSprint to dynamically update the resting levels.

```
+------------------+             +--------------------+             +----------------------+
|  Binance API     |             |  Python Connector  |             |  BlockSprint Engine  |
|  (REST + WS)     |             |  Bridge Daemon     |             |  (C++ API Server)    |
+--------+---------+             +---------+----------+             +----------+-----------+
         |                                 |                                   |
         | -- Get All Active Tickers ----> |                                   |
         |                                 | -- Initialize Symbols ----------> |
         |                                 |                                   |
         | -- Get REST L2 Snapshot ------->|                                   |
         |                                 | -- Push Initial Book Snapshot --> |
         |                                 |                                   |
         | == Open WebSocket Stream =======|                                   |
         | -- Stream Real-Time Depths ---->|                                   |
         |                                 | -- Forward JSON Delta updates --> |
         |                                 |                                   |
```

---

## 2. Python Bridge Setup & Symbol Discovery

The Python bridge service coordinates connections, queries symbols, and handles translation.

### 1. Active Symbol Discovery & Filtering
To select the target markets (e.g., USDT pairs with high liquidity), we retrieve all tickers on startup:

```python
import pandas as pd
from binance.client import Client

client = Client(api_key, api_secret)

# 1. Fetch all tickers from Binance
tickers = client.get_all_tickers()
df = pd.DataFrame(tickers)

# 2. Filter for USDT markets only
df_usdt = df[df['symbol'].str.endswith('USDT')]

# 3. Limit to major active markets to conserve bandwidth
active_symbols = df_usdt['symbol'].head(20).tolist() # e.g., ['BTCUSDT', 'ETHUSDT', 'SOLUSDT', ...]
```

### 2. Loading the REST Snapshot
Before listening to the WebSocket stream, make a single REST query to populate the book:

```python
import requests

url = "https://api1.binance.com/api/v3/depth"

for symbol in active_symbols:
    # 1. Convert "ETHUSDT" -> "ETH-USDT"
    formatted_symbol = symbol.replace("USDT", "-USDT")
    
    # 2. Query the REST endpoint
    params = {"symbol": symbol, "limit": "10"}
    response = requests.get(url, params=params)
    data = response.json()
    
    # data format:
    # {
    #   "lastUpdateId": 78191178910,
    #   "bids": [ ["1584.1100", "8.0951"], ["1584.1000", "0.0229"] ],
    #   "asks": [ ["1584.1200", "22.7782"], ["1584.1300", "7.0012"] ]
    # }
    
    # 3. Push this snapshot to C++ REST endpoint /api/sync-snapshot
    requests.post("http://127.0.0.1:8080/api/sync-snapshot", json={
        "symbol": formatted_symbol,
        "bids": data["bids"],
        "asks": data["asks"]
    })
```

---

## 3. Real-Time WebSocket Synchronization

Once bootstrapped, connect a WebSocket client to Binance to stream multiplexed delta updates:

* **Endpoint**: `wss://stream.binance.com:9443/stream?streams=ethusdt@depth10@100ms/btcusdt@depth10@100ms/...`

```python
import asyncio
import websockets
import json

async def sync_binance_websocket(active_symbols):
    # Construct multiplexed stream URI
    streams = "/".join([f"{s.lower()}@depth10@100ms" for s in active_symbols])
    uri = f"wss://stream.binance.com:9443/stream?streams={streams}"
    
    # Connect to Binance and BlockSprint simultaneously
    async with websockets.connect(uri) as binance_ws, \
               websockets.connect("ws://127.0.0.1:8080/ws/orders") as engine_ws:
               
        while True:
            frame = await binance_ws.recv()
            msg = json.loads(frame)
            
            stream_name = msg["stream"] # e.g. "ethusdt@depth10@100ms"
            data = msg["data"]
            
            # Map symbol "ethusdt" -> "ETH-USDT"
            raw_symbol = stream_name.split("@")[0].upper()
            formatted_symbol = raw_symbol.replace("USDT", "-USDT")
            
            # Format update command for BlockSprint
            update_cmd = {
                "action": "sync_external_book",
                "symbol": formatted_symbol,
                "bids": data["bids"], # [["1584.11", "8.095"], ...]
                "asks": data["asks"]  # [["1584.12", "22.778"], ...]
            }
            
            # Forward update to C++ engine
            await engine_ws.send(json.dumps(update_cmd))
```

---

## 4. Required C++ Modifications

To consume this live data, BlockSprint needs new handlers inside the matching engine core:

### 1. Model Updates (`src/models.hpp`)
No changes are required to the basic structural model. The JSON parser will unpack price and quantity strings directly.

### 2. API Server Handlers (`src/api_server.cpp`)
Extend `on_message` for `/ws/orders` or add a new HTTP REST endpoint `/api/sync-snapshot` to parse incoming sync packets:

```cpp
// Add to api_server.cpp (WebSocket message parser)
if (action == "sync_external_book") {
    std::string symbol = body.at("symbol").get<std::string>();
    auto bids = body.at("bids");
    auto asks = body.at("asks");
    
    MatchingEngineManager::getInstance().sync_external_liquidity(symbol, bids, asks);
}
```

### 3. Engine Manager & Order Book Mutation (`src/order_book.cpp`)
When updating the local book using Binance values, we treat them as resting limit orders belonging to a system user (`BINANCE_L2_MAKER`). We apply the **Zero-Quantity Rule**:

```cpp
void OrderBook::sync_external_levels(const nlohmann::json& bids, const nlohmann::json& asks) {
    // 1. Process Bids
    for (const auto& bid : bids) {
        double price = std::stod(bid[0].get<std::string>());
        double qty = std::stod(bid[1].get<std::string>());
        
        // Find existing Binance order at this price level
        std::string order_id = "BINANCE_BID_" + std::to_string(price);
        
        if (qty == 0.0) {
            // Cancel and remove level if quantity is zero
            cancel_order(order_id);
        } else {
            // Otherwise, update or create resting order
            std::vector<Trade> dummy;
            modify_order(order_id, qty, price, dummy);
        }
    }
    
    // 2. Process Asks (Same logic using "BINANCE_ASK_" prefix)
}
```

---

## 5. Outbound Hedging (Brokerage Flow)

When an internal user submits an order that matches against a `BINANCE_L2_MAKER` order:
1. BlockSprint executes the match locally and generates a `Trade` event.
2. The trade callback intercepts the transaction. If `maker_order_id` starts with `"BINANCE_"`, it calls a background worker:
   ```python
   # Triggered in Python connector bridge listening to the Trades stream
   if trade["maker_order_id"].startswith("BINANCE_"):
       # Send execution order to Binance REST API
       client.create_order(
           symbol=trade["symbol"].replace("-", ""),
           side=Client.SIDE_BUY if trade["aggressor_side"] == "sell" else Client.SIDE_SELL,
           type=Client.ORDER_TYPE_LIMIT,
           timeInForce=Client.TIME_IN_FORCE_IOC,
           quantity=trade["quantity"],
           price=trade["price"]
       )
   ```
3. This completes the hedging loop, offloading market risk onto Binance in real-time.
