# Command Execution Guide

This document lists the sequence of terminal commands needed to build, test, run, and benchmark the C++ Crypto Trading Engine. All commands must be run from the root directory of the workspace (`c:\Users\Lenovo\Desktop\Crypto_Trading_Engine`).

---

## Step 1: Configure the Build System
Create the build files using CMake and detect your C++ compiler (MSVC):
```powershell
cmake -B build
```

---

## Step 2: Build the Targets
Compile both the production matching engine and the unit testing binaries in **Release** configuration for maximum speed:
```powershell
cmake --build build --config Release
```
This produces two executables under the `build\Release\` directory:
- `crypto_matching_engine.exe` (Production API and Matching Engine server)
- `run_tests.exe` (Engine unit test suite)

---

## Step 3: Run the Unit Tests
Before launching the server, execute the unit test binary to ensure the FIFO queue priority, trade-through protection, cancellations, and order types are behaving correctly:
```powershell
.\build\Release\run_tests.exe
```
*Expected Output*: Displays logs confirming all 6 test cases passed.

---

## Step 4: Start the Production Trading Engine Server
Run the production server. This will start listening on port `8080` for incoming REST HTTP requests and WebSocket streams:
```powershell
.\build\Release\crypto_matching_engine.exe
```
*Note*: Keep this terminal window open so the server continues running. If you want to shut down the server, press `Ctrl+C`.

---

## Step 5: Setup the Python Test Clients (Separate Terminal)
Open a new terminal window at the project root (`c:\Users\Lenovo\Desktop\Crypto_Trading_Engine`) to run python scripts.

Since `uv` is installed, install the required `websockets` library inside the environment:
```powershell
uv pip install websockets
# or if using a standard python env:
pip install websockets
```

---

## Step 6: Run the Performance Benchmark
With the C++ server running in Step 4, run the load-test script to measure the end-to-end throughput of WebSockets:
```powershell
uv run scripts/perf_test.py
# or if using a standard python env:
python scripts/perf_test.py
```
*Expected Output*: Measures time elapsed and prints throughput in `orders/sec` (e.g. `> 2000 orders/sec`).

---

## Step 7: Run the Client Subscription Demo
Run the client demo to see real-time streaming updates. It connects to the trades and market data WebSocket feeds, submits some Buy/Sell orders via HTTP POST, and displays streamed BBO/depth updates:
```powershell
uv run scripts/client_demo.py
# or if using a standard python env:
python scripts/client_demo.py
```
