# High-Performance C++ Cryptocurrency Matching Engine Implementation Plan

This document outlines the detailed architecture and implementation plan for building a high-performance cryptocurrency matching engine purely in **C++17**, using **Visual Studio 2022 / MSVC** and modern CMake.

## User Review Required

> [!IMPORTANT]
> **Key Design Choices & Constraints:**
> - **Language**: We will implement the engine in **C++17**.
> - **Build System**: We will use **CMake** generating Visual Studio project files.
> - **Dependencies**: To avoid configuration issues or slow compile-time github downloads, we have already downloaded the header-only dependencies locally in the `external/` directory:
>   - [nlohmann/json](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/external/nlohmann/json.hpp) (v3.11.3)
>   - [asio](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/external/asio) (v1.30.2 - standalone, header-only, no Boost required)
>   - [websocketpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/external/websocketpp) (v0.8.2 - header-only)
> - **Threading Model**: The matching engine core will run in a single-threaded event loop (sequential consistency) to eliminate locking overhead, data races, and context-switching. Network I/O and WebSocket broadcasts will run asynchronously on Asio's event loop.
> - **Performance & Data Structures**:
>   - **`std::map`** (Red-Black tree) for price levels. Bids sorted in descending order (`std::greater`), asks in ascending order (`std::less`).
>   - **`std::list`** for price level order queues (FIFO).
>   - **`std::unordered_map`** for order lookups by ID.
>   - **`O(1)` Cancellations**: We will store an iterator in the `Order` struct pointing to its position in the price level's `std::list`. When a cancellation request arrives, we locate the order in the `std::unordered_map` in `O(1)` and erase it from the list in `O(1)`, avoiding any linear search or lazy deletion.

## Open Questions

> [!NOTE]
> There are no blocking open questions. We have verified the MSVC compilation environment with our local `external/` dependencies successfully.

---

## Proposed Changes

We will organize the project into a clean, modular structure. Below is the breakdown of components.

### 1. Build and Dependencies

#### [MODIFY] [external/](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/external/)
Already populated with:
- `external/nlohmann/json.hpp`
- `external/asio/`
- `external/websocketpp/`

#### [NEW] [CMakeLists.txt](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/CMakeLists.txt)
CMake configuration mapping local include directories and building the engine and test executables:
- Targets: `crypto_matching_engine` (main executable), `run_tests` (unit tests).
- Compiler flags: Standard C++17, MSVC specific warnings and optimizations (`/O2`, `/W4`).

---

### 2. Core Models

#### [NEW] [models.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/models.hpp)
Data structures representing domain concepts:
- `enum class OrderType { LIMIT, MARKET, IOC, FOK };`
- `enum class Side { BUY, SELL };`
- `struct Order`: Contains ID, symbol, side, type, price, quantity, remaining quantity, timestamp, is_cancelled, and a list iterator back-pointer.
- `struct Trade`: Contains execution reports with price, quantity, maker/taker order IDs, aggressor side, and timestamp.
- JSON serialization: Define `to_json` / `from_json` for seamless conversion with `nlohmann/json`.

---

### 3. Matching Engine Core

#### [NEW] [order_book.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/order_book.hpp) & [order_book.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/order_book.cpp)
Implements price-time priority matching logic for a single trading pair.
- **Data Structures**:
  - `bids`: `std::map<double, PriceLevel, std::greater<double>>`
  - `asks`: `std::map<double, PriceLevel, std::less<double>>`
  - `orders_lookup`: `std::unordered_map<std::string, std::shared_ptr<Order>>`
- **Key Methods**:
  - `submit_order(std::shared_ptr<Order> order) -> std::vector<Trade>`: Core matching algorithm.
  - `cancel_order(const std::string& order_id) -> bool`: O(1) order cancellation using the iterator back-pointer.
  - `get_bbo() -> nlohmann::json`: Calculates Best Bid and Offer.
  - `get_l2_depth(int depth = 10) -> nlohmann::json`: Compiles top bids and asks.
- **Matching Details**:
  - *FOK validation*: Simulated walk through the order book to ensure the entire quantity can be filled. If not, the order is rejected immediately.
  - *FIFO matching*: Loops through price levels and front of `std::list` queues, creating `Trade` structures.

#### [NEW] [matching_engine.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/matching_engine.hpp) & [matching_engine.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/matching_engine.cpp)
Manages multiple `OrderBook` instances (one per symbol, e.g. "BTC-USDT") and exposes thread-safe order routing.

---

### 4. API & WebSocket Server

#### [NEW] [api_server.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/api_server.hpp) & [api_server.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/api_server.cpp)
A unified HTTP/REST and WebSocket server built using WebSocket++ and standalone Asio.
- **REST Endpoints** (handled in HTTP callback):
  - `POST /api/orders`: Submit new order (parses JSON body, executes trade, returns response).
  - `DELETE /api/orders`: Cancel order (requires JSON body or query param).
- **WebSocket Endpoints**:
  - `/ws/orders`: Allows bi-directional submission of orders.
  - `/ws/market-data`: Client registers to receive L2/BBO feeds for symbols.
  - `/ws/trades`: Client registers to receive trade execution updates.
- **Broadcast System**:
  - Manages active socket connections.
  - Broadcasts market depth updates and trades asynchronously.

---

### 5. Diagnostics & Application Entry

#### [NEW] [logger.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/logger.hpp)
Standardized file and console logger for system events, trading records, and errors.

#### [NEW] [main.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/main.cpp)
Main entry point. Initializes the matching engine manager, starts the WebSocket/HTTP server, and spins up the background thread.

---

### 6. Testing & Validation

#### [NEW] [test_harness.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/tests/test_harness.hpp) & [main_test.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/tests/main_test.cpp)
A lightweight unit testing harness verifying matching invariants:
- Price-time priority verification.
- Internal trade-through protection.
- Market, Limit, IOC, and FOK order types.
- BBO calculations and updates.
- Cancellations and modifications.

#### [NEW] [perf_test.py](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/scripts/perf_test.py)
A load testing client in Python that connects via WebSockets/REST, throws thousands of mock orders at the running C++ engine, and prints statistics.

---

## Verification Plan

### Automated Tests
1. **Configure & Build Tests Target**:
   - `cmake -B build`
   - `cmake --build build --config Release`
2. **Run C++ Unit Tests**:
   - `.\build\Release\run_tests.exe`
   - Ensure all matching engine tests pass.

### Manual & Performance Verification
1. Run the server: `.\build\Release\crypto_matching_engine.exe`
2. Run `python scripts/perf_test.py` to assert the engine processes >1000 orders/sec and streams updates with low latency.
3. Validate client feeds using `python scripts/client_demo.py`.
