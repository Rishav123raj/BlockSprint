import asyncio
import websockets
import json
import time
import random
import os
import logging
import urllib.request

os.makedirs("logs", exist_ok=True)
logging.basicConfig(
    filename="logs/perf_test.log",
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    filemode="w" # Overwrite each run for clean stats
)

def reset_engine():
    """Call C++ REST endpoint /api/reset to ensure clean state before benchmark."""
    try:
        url = "http://127.0.0.1:8080/api/reset"
        req = urllib.request.Request(url, method="POST")
        with urllib.request.urlopen(req, timeout=2) as response:
            logging.info("Matching engine state reset successfully before benchmark.")
    except Exception as e:
        logging.warning(f"Could not reset matching engine: {e}")

def get_bbo_price(symbol):
    """Retrieve current BBO mid-price to run the benchmark around real spreads if applicable."""
    try:
        url = f"http://127.0.0.1:8080/api/bbo?symbol={symbol}"
        with urllib.request.urlopen(url, timeout=2) as response:
            data = json.loads(response.read().decode())
            ask = float(data.get("ask_price", 0.0))
            bid = float(data.get("bid_price", 0.0))
            if ask > 0 and bid > 0:
                return (ask + bid) / 2.0
    except Exception:
        pass
    return 100.0

async def send_orders(websocket, num_orders, symbol, baseline_price):
    for i in range(num_orders):
        # Generate some buy/sell limit orders around the baseline price
        side = "buy" if random.random() > 0.5 else "sell"
        price = round(baseline_price + random.uniform(-2.0, 2.0), 2)
        qty = round(random.uniform(0.1, 10.0), 2)

        order = {
            "action": "submit",
            "order_id": f"PERF_{i}",
            "symbol": symbol,
            "order_type": "limit",
            "side": side,
            "price": f"{price}",
            "quantity": f"{qty}"
        }
        await websocket.send(json.dumps(order))

async def receive_confirmations(websocket, num_orders):
    confirmations = 0
    trades_count = 0
    while confirmations < num_orders:
        resp = await websocket.recv()
        data = json.loads(resp)
        if data.get("status") in ("success", "error"):
            confirmations += 1
            trades_count += len(data.get("trades", []))
    return trades_count

async def run_benchmark(num_orders, symbol="TEST-USDT"):
    # 1. Clear out order books first
    reset_engine()
    
    # 2. Get dynamic baseline price
    baseline_price = get_bbo_price(symbol)
    
    uri = "ws://127.0.0.1:8080/ws/orders"
    logging.info(f"Connecting to {uri}...")
    logging.info(f"Running benchmark on {symbol} with baseline price {baseline_price:.2f}")
    
    async with websockets.connect(uri) as websocket:
        logging.info("Connected! Generating orders...")
        
        start_time = time.perf_counter()
        
        # Run sender and receiver concurrently to saturate throughput
        send_task = asyncio.create_task(send_orders(websocket, num_orders, symbol, baseline_price))
        recv_task = asyncio.create_task(receive_confirmations(websocket, num_orders))
        
        await send_task
        trades_filled = await recv_task
        
        end_time = time.perf_counter()
        total_time = end_time - start_time
        throughput = num_orders / total_time
        
        logging.info("=== Benchmark Results ===")
        logging.info(f"Total Orders Submitted: {num_orders}")
        logging.info(f"Total Matches (Trades): {trades_filled}")
        logging.info(f"Total Time Taken:      {total_time:.4f} seconds")
        logging.info(f"Throughput:            {throughput:.2f} orders/sec")
        logging.info("=========================")

if __name__ == "__main__":
    # Benchmark on TEST-USDT with 5000 orders
    asyncio.run(run_benchmark(5000, "TEST-USDT"))
