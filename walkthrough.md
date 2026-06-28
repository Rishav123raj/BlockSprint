# Cryptocurrency Matching Engine Walkthrough

I have implemented a high-performance, low-dependency cryptocurrency matching engine purely in **C++17** using Visual Studio 2022 / MSVC and CMake. 

Here is a summary of the implementation, components, verification, and benchmark results.

---

## Technical Architecture & Design Invariants

- **Zero-Copy Order Cancellation**: Using C++ standard library `std::list` to store active order queues and keeping a list iterator back-pointer in each `Order` struct. Cancellations are mapped in an `std::unordered_map` and removed in **O(1)** time, completely avoiding costly scans or lazy deletion.
- **Price-Time Priority (FIFO)**: Implemented using sorted red-black tree structures: `std::map<double, PriceLevel, std::greater<double>>` for bids (descending) and `std::map<double, PriceLevel, std::less<double>>` for asks (ascending).
- **Internal Trade-Through Protection**: Sweeps the spread starting from the best available bid/ask price first. No orders are filled at a worse price if a better price is available.
- **Unified API Server**: Integrated HTTP REST routes and WebSocket feeds using the header-only **WebSocket++** library and standalone **Asio** (running without Boost library requirements).

---

## Source Components

All components are located in the workspace directory:
- [CMakeLists.txt](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/CMakeLists.txt): Configures build targets for the server and the test harness.
- [models.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/models.hpp): Contains the `Order`, `Trade`, `Side`, and `OrderType` struct declarations and JSON mapping.
- [logger.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/logger.hpp): Thread-safe diagnostics logger. Modified LogLevel to avoid macro clashes with Windows `ERROR` codes.
- [order_book.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/order_book.hpp) & [order_book.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/order_book.cpp): Price priority matching, FOK simulated walks, and order modifications.
- [matching_engine.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/matching_engine.hpp) & [matching_engine.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/matching_engine.cpp): Manager routing requests to symbol books and invoking callbacks.
- [api_server.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/api_server.hpp) & [api_server.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/api_server.cpp): Rest endpoints and WebSocket subscription handlers.
- [main.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/main.cpp): Program entry point hooks matching signals and binds feeds.

---

## Verification Results

### 1. Automated Unit Tests
A lightweight test suite was written in [tests/main_test.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/tests/main_test.cpp) verifying matching core invariants.

All unit tests compiled and passed successfully:
```log
[INFO] === Running Crypto Trading Engine C++ Unit Tests ===
[INFO] Running test: price_time_priority
[INFO] PASS: price_time_priority
[INFO] Running test: internal_trade_through_protection
[INFO] PASS: internal_trade_through_protection
[INFO] Running test: ioc_order_handling
[INFO] PASS: ioc_order_handling
[INFO] Running test: fok_order_handling
[INFO] PASS: fok_order_handling
[INFO] Running test: order_cancellation
[INFO] PASS: order_cancellation
[INFO] Running test: order_modification
[INFO] PASS: order_modification
[INFO] === Unit Test Results ===
[INFO] Passes: 6
[INFO] Failures: 0
```

### 2. High-Throughput Performance Benchmark
The performance script [scripts/perf_test.py](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/scripts/perf_test.py) benchmarked the server by executing 5,000 trades over raw WebSocket TCP loops:

```log
Connecting to ws://127.0.0.1:8080/ws/orders...
Connected! Generating orders...

=== Benchmark Results ===
Total Orders Submitted: 5000
Total Matches (Trades): 3818
Total Time Taken:      2.0910 seconds
Throughput:            2391.15 orders/sec
=========================
```
The benchmark demonstrates a throughput of **2391.15 orders/sec** end-to-end, exceeding the target of >1000 orders/sec.

### 3. Client Integration Demo
The integration script [scripts/client_demo.py](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/scripts/client_demo.py) connects to the market data feed and streams L2 depth, BBO, and executions correctly as orders are matched.
