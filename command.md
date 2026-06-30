# Aggressive Testing & Command Execution Guide

This document lists the exact sequence of terminal commands needed to build, test, execute, and aggressively benchmark the BlockSprint Trading Engine. All commands must be run from the root directory of the workspace (`c:\Users\Lenovo\Desktop\Crypto_Trading_Engine`).

---

## Phase 1: Environment Provisioning

Before executing the binaries, install all C++ dependencies (cached locally) and Python requirements:

```powershell
# 1. Install Python dependencies inside your environment
uv add requests python-dotenv websockets
# or standard pip:
pip install requests python-dotenv websockets

# 2. Configure credentials
# Ensure you have a .env file in the root containing:
# BINANCE_API_KEY=your_key
# BINANCE_API_SECRET=your_secret
```

---

## Phase 2: Compilation

Compile both the production matching engine and the unit testing binaries in **Release** configuration for maximum execution speed and compiler optimizations:

```powershell
# 1. Configure CMake build generation
cmake -B build

# 2. Build the project targets
cmake --build build --config Release
```
This produces two executables under the `build\Release\` directory:
- `crypto_matching_engine.exe` (Production API and Matching Engine server)
- `run_tests.exe` (Engine unit test suite)

---

## Phase 3: Automated Invariant Testing

Before launching any services, run the unit test binary to ensure the FIFO queue priority, trade-through protection, cancellations, and order types are behaving correctly:

```powershell
# Run the test harness
.\build\Release\run_tests.exe
```
*Expected Verification*: Check [`logs/matching_engine.log`](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/logs/matching_engine.log). It must contain:
```text
Passes: 6
Failures: 0
```

---

## Phase 4: Production Engine Server Startup

Run the production server. This will start listening on port `8080` for incoming REST HTTP requests and WebSocket streams:

```powershell
# Start the engine server
.\build\Release\crypto_matching_engine.exe
```
*Note*: Keep this terminal window open. If you want to shut down the server, press `Ctrl+C`.

---

## Phase 5: Run the Live Binance Sync (Second Terminal)

Open a new terminal at the root and run the connector to sync BlockSprint with the live market data from Binance:

```powershell
# Run the connector
uv run scripts/binance_connector.py
```
*Expected Verification*: The connector should boot, select active tickers, output REST snapshots, and start streaming WebSocket deltas to the C++ server.

---

## Phase 6: Aggressive Verification of Live Sync (Third Terminal)

Verify that the C++ matching engine is correctly executing updates and maintaining real-time order books:

### 1. Verify Best Bid/Offer (BBO) Price Sync
```powershell
curl "http://127.0.0.1:8080/api/bbo?symbol=BTC-USDT"
```
*Expected Result*: Returns a JSON structure showing live bid/ask spreads matching the live price of Bitcoin.

### 2. Verify L2 Queue Depth
```powershell
curl "http://127.0.0.1:8080/api/depth?symbol=BTC-USDT"
```
*Expected Result*: Returns lists of top 10 bids/asks and their quantities currently resting in the C++ memory book.

### 3. Verify Log File Updates
Verify that logs are correctly routed to the log folder and timestamps are converted to your system local time (IST):
- Check [`logs/matching_engine.log`](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/logs/matching_engine.log) for trade matches and synchronization updates.

---

## Phase 7: Heavy Performance Load Benchmarking

Run the WebSocket performance load-testing script to stress-test the WebSocket parsing speed of the C++ engine:

```powershell
# Run the benchmark (submits 5,000 orders)
uv run scripts/perf_test.py
```
*Expected Verification*: Check [`logs/perf_test.log`](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/logs/perf_test.log) to ensure throughput meets the high-performance goal (e.g. `Throughput: > 1500 orders/sec`).

---

## Phase 8: End-to-End Client Demo Execution

Run the simulation script to test simultaneous WebSocket subscriptions and REST order submissions:

```powershell
# Run the client simulation
uv run scripts/client_demo.py
```
*Expected Verification*: Check [`logs/client_demo.log`](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/logs/client_demo.log) to verify trades are executing correctly against the live spreads.
