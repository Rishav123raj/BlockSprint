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
            print("Matching engine state reset successfully before benchmark.")
    except Exception as e:
        logging.warning(f"Could not reset matching engine: {e}")
        print(f"Warning: Could not reset matching engine: {e}")

def get_bbo_price(symbol):
    """Retrieve current BBO mid-price to run the benchmark around real spreads if applicable.
    Tries Binance public API first, then local engine, and defaults to 100.0.
    """
    # 1. Try public Binance API directly
    try:
        binance_symbol = symbol.replace("-", "")
        url = f"https://api.binance.com/api/v3/ticker/price?symbol={binance_symbol}"
        with urllib.request.urlopen(url, timeout=2) as response:
            data = json.loads(response.read().decode())
            price = float(data.get("price", 0.0))
            if price > 0:
                return price
    except Exception:
        pass

    # 2. Try local matching engine
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

async def send_orders(websocket, num_orders, symbol, baseline_price, send_times, sem):
    for i in range(num_orders):
        # Acquire semaphore to limit max in-flight orders
        await sem.acquire()
        
        side = "buy" if random.random() > 0.5 else "sell"
        price = round(baseline_price + random.uniform(-2.0, 2.0), 2)
        qty = round(random.uniform(0.1, 10.0), 2)

        order_id = f"PERF_{i}"
        order = {
            "action": "submit",
            "order_id": order_id,
            "symbol": symbol,
            "order_type": "limit",
            "side": side,
            "price": f"{price}",
            "quantity": f"{qty}"
        }
        
        # Record exact timestamp of send
        send_times[order_id] = time.perf_counter()
        await websocket.send(json.dumps(order))

async def receive_confirmations(websocket, num_orders, send_times, latencies, sem):
    confirmations = 0
    trades_count = 0
    success_count = 0
    error_count = 0
    
    while confirmations < num_orders:
        resp = await websocket.recv()
        recv_time = time.perf_counter()
        data = json.loads(resp)
        
        status = data.get("status")
        if status in ("success", "error"):
            confirmations += 1
            if status == "success":
                success_count += 1
            else:
                error_count += 1
                
            trades_count += len(data.get("trades", []))
            
            # Release semaphore as an order has been confirmed
            sem.release()
            
            order_id = data.get("order_id")
            if order_id in send_times:
                # Latency in seconds
                latencies.append(recv_time - send_times[order_id])
                
    return trades_count, success_count, error_count

async def run_benchmark(num_orders, symbol="TEST-USDT"):
    # 1. Clear out order books first
    reset_engine()
    
    # 2. Get dynamic baseline price
    baseline_price = get_bbo_price(symbol)
    
    uri = "ws://127.0.0.1:8080/ws/orders"
    msg_connect = f"Connecting to {uri}..."
    logging.info(msg_connect)
    print(msg_connect)
    
    msg_start = f"Running benchmark on {symbol} with baseline price {baseline_price:.2f}"
    logging.info(msg_start)
    print(msg_start)
    
    async with websockets.connect(uri) as websocket:
        logging.info("Connected! Generating and sending orders...")
        print("Connected! Generating and sending orders...")
        
        # In-flight sliding window of 100 orders to prevent client-side queue bloat
        sem = asyncio.Semaphore(100)
        send_times = {}
        latencies = []
        
        start_time = time.perf_counter()
        
        # Run sender and receiver concurrently to saturate throughput
        send_task = asyncio.create_task(send_orders(websocket, num_orders, symbol, baseline_price, send_times, sem))
        recv_task = asyncio.create_task(receive_confirmations(websocket, num_orders, send_times, latencies, sem))
        
        await send_task
        trades_filled, success_count, error_count = await recv_task
        
        end_time = time.perf_counter()
        total_time = end_time - start_time
        throughput = num_orders / total_time
        
        # Convert latencies to microseconds
        latencies_us = [l * 1_000_000 for l in latencies]
        latencies_us.sort()
        
        total_lat = len(latencies_us)
        avg_latency = sum(latencies_us) / total_lat if total_lat else 0.0
        p50 = latencies_us[int(total_lat * 0.50)] if total_lat else 0.0
        p90 = latencies_us[int(total_lat * 0.90)] if total_lat else 0.0
        p95 = latencies_us[int(total_lat * 0.95)] if total_lat else 0.0
        p99 = latencies_us[int(total_lat * 0.99)] if total_lat else 0.0
        p999 = latencies_us[int(total_lat * 0.999)] if total_lat else 0.0
        min_lat = latencies_us[0] if total_lat else 0.0
        max_lat = latencies_us[-1] if total_lat else 0.0
        
        report = []
        report.append("==================================================")
        report.append("          BlockSprint Benchmark Metrics Report    ")
        report.append("==================================================")
        report.append(f"Target Symbol:          {symbol}")
        report.append(f"Total Orders Sent:      {num_orders}")
        report.append(f"Confirmations Received: {total_lat}")
        report.append(f"  - Success:            {success_count}")
        report.append(f"  - Error/Rejected:     {error_count}")
        report.append(f"Total Matches (Trades): {trades_filled}")
        report.append(f"Elapsed Time:           {total_time:.4f} seconds")
        report.append(f"Sustained Throughput:   {throughput:.2f} orders/sec")
        report.append("--------------------------------------------------")
        report.append("Round-Trip Latency Statistics (WebSocket RTT):")
        report.append(f"  - Min Latency:        {min_lat:.2f} µs")
        report.append(f"  - Average Latency:    {avg_latency:.2f} µs")
        report.append(f"  - Median (p50):       {p50:.2f} µs")
        report.append(f"  - 90th Percentile:    {p90:.2f} µs")
        report.append(f"  - 95th Percentile:    {p95:.2f} µs")
        report.append(f"  - 99th Percentile:    {p99:.2f} µs")
        report.append(f"  - 99.9th Percentile:  {p999:.2f} µs")
        report.append(f"  - Max Latency:        {max_lat:.2f} µs")
        report.append("==================================================")
        
        for line in report:
            logging.info(line)
            print(line)

if __name__ == "__main__":
    # Benchmark on TEST-USDT with 5000 orders
    asyncio.run(run_benchmark(5000, "ETH-USDT"))
