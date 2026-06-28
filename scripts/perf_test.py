import asyncio
import websockets
import json
import time
import random

async def send_orders(websocket, num_orders):
    for i in range(num_orders):
        # Generate some buy/sell limit orders around a baseline price of 100
        side = "buy" if random.random() > 0.5 else "sell"
        # We vary the price slightly to trigger execution crossings
        price = round(100.0 + random.uniform(-2.0, 2.0), 2)
        qty = round(random.uniform(0.1, 10.0), 2)

        order = {
            "action": "submit",
            "order_id": f"PERF_{i}",
            "symbol": "BTC-USDT",
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

async def run_benchmark(num_orders):
    uri = "ws://127.0.0.1:8080/ws/orders"
    print(f"Connecting to {uri}...")
    
    async with websockets.connect(uri) as websocket:
        print("Connected! Generating orders...")
        
        start_time = time.perf_counter()
        
        # Run sender and receiver concurrently to saturate throughput
        send_task = asyncio.create_task(send_orders(websocket, num_orders))
        recv_task = asyncio.create_task(receive_confirmations(websocket, num_orders))
        
        await send_task
        trades_filled = await recv_task
        
        end_time = time.perf_counter()
        total_time = end_time - start_time
        throughput = num_orders / total_time
        
        print("\n=== Benchmark Results ===")
        print(f"Total Orders Submitted: {num_orders}")
        print(f"Total Matches (Trades): {trades_filled}")
        print(f"Total Time Taken:      {total_time:.4f} seconds")
        print(f"Throughput:            {throughput:.2f} orders/sec")
        print("=========================")

if __name__ == "__main__":
    # Test with 5000 orders
    asyncio.run(run_benchmark(5000))
