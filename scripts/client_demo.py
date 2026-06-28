import asyncio
import websockets
import json
import urllib.request
import urllib.parse
import threading
import time
import os
import logging

os.makedirs("logs", exist_ok=True)
logging.basicConfig(
    filename="logs/client_demo.log",
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    filemode="w"
)

def submit_order_http(side, price, qty):
    url = "http://127.0.0.1:8080/api/orders"
    data = {
        "symbol": "BTC-USDT",
        "order_type": "limit",
        "side": side,
        "price": str(price),
        "quantity": str(qty)
    }
    req = urllib.request.Request(
        url, 
        data=json.dumps(data).encode("utf-8"), 
        headers={"Content-Type": "application/json"},
        method="POST"
    )
    try:
        with urllib.request.urlopen(req) as response:
            res_body = response.read().decode("utf-8")
            logging.info(f"[HTTP Response] Order Submitted: {res_body}")
    except Exception as e:
        logging.error(f"[HTTP Error] Submission failed: {e}")

async def listen_market_data():
    uri = "ws://127.0.0.1:8080/ws/market-data"
    logging.info(f"[WS Client] Connecting to Market Data Feed at {uri}...")
    try:
        async with websockets.connect(uri) as websocket:
            logging.info("[WS Client] Connected to Market Data Feed!")
            while True:
                msg = await websocket.recv()
                data = json.loads(msg)
                logging.info(f"[Market Data Update] {json.dumps(data, indent=2)}")
    except Exception as e:
        logging.error(f"[WS Client] Market Data error: {e}")

async def listen_trades():
    uri = "ws://127.0.0.1:8080/ws/trades"
    logging.info(f"[WS Client] Connecting to Trades Feed at {uri}...")
    try:
        async with websockets.connect(uri) as websocket:
            logging.info("[WS Client] Connected to Trades Feed!")
            while True:
                msg = await websocket.recv()
                data = json.loads(msg)
                logging.info(f"[Trade Executed Update] {json.dumps(data, indent=2)}")
    except Exception as e:
        logging.error(f"[WS Client] Trades error: {e}")

async def main():
    # Run listeners in the background
    md_task = asyncio.create_task(listen_market_data())
    tr_task = asyncio.create_task(listen_trades())
    
    # Wait for connections to establish
    await asyncio.sleep(2)
    
    # Submit some demo orders via HTTP in a separate thread/task
    logging.info("--- Submitting resting Buy order (100.0, qty 1.5) ---")
    submit_order_http("buy", 100.0, 1.5)
    await asyncio.sleep(1)

    logging.info("--- Submitting resting Buy order (100.0, qty 2.5) ---")
    submit_order_http("buy", 100.0, 2.5)
    await asyncio.sleep(1)

    logging.info("--- Submitting crossing Sell order (99.0, qty 2.0) ---")
    # This should match 1.5 against first buy order, then 0.5 against second buy order
    submit_order_http("sell", 99.0, 2.0)
    await asyncio.sleep(2)
    
    logging.info("Shutting down demo...")
    md_task.cancel()
    tr_task.cancel()

if __name__ == "__main__":
    asyncio.run(main())
