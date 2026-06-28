# Cryptocurrency Trading Engine - File-by-File Explanation

This document provides a detailed, sequential, file-by-file breakdown of the high-performance C++ matching engine. Files are listed in logical order of compilation and dependency (bottom-up).

---

## 1. Build Configuration

### [CMakeLists.txt](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/CMakeLists.txt)
* **Purpose**: Orchestrates compilation, includes dependencies, sets compiler flags, and defines executable targets.
* **Key Details**:
  - Configures **C++17** standard (`CMAKE_CXX_STANDARD 17`).
  - Sets up headers for locally cached dependency libraries under `external/`:
    - `external/nlohmann/json.hpp` (Modern JSON parsing)
    - `external/asio/asio/include` (Standalone networking event loop)
    - `external/websocketpp` (Header-only WebSocket server)
  - Adds macro compiler flags:
    - `ASIO_STANDALONE`: Directs Asio to compile without compiling or searching for the Boost library.
    - `_WEBSOCKETPP_CPP11_STL_`: Informs WebSocket++ to use standard C++11 library components (like threads, memory management, and smart pointers) rather than Boost variants.
  - Controls MSVC compiler features:
    - `/W4`: High warning level.
    - `/EHsc`: Safe synchronous exception handling.
    - `/O2`: Maximum speed optimization in Release build configurations.
  - Defines two binary build targets:
    1. `crypto_matching_engine`: The production API Server.
    2. `run_tests`: The standalone test suite executable.

---

## 2. Core Structures & Diagnostics

### [src/models.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/models.hpp)
* **Purpose**: Defines standard enumerations and structures representing trading entities.
* **Key Details**:
  - **`OrderType`**: Enum containing `LIMIT`, `MARKET`, `IOC`, and `FOK` types.
  - **`Side`**: Enum containing `BUY` and `SELL` sides.
  - **`Order`**: Represents a single order. Holds quantity, price, timestamp, and a flag indicating if it was cancelled. Crucially, it includes:
    ```cpp
    std::list<std::shared_ptr<Order>>::iterator list_it;
    bool has_iterator = false;
    ```
    This iterator stores the exact location of the order inside the `PriceLevel` double-linked list. It permits **O(1) deletion** of resting orders during cancellations.
  - **`Trade`**: Represents execution fills. Contains trade ID, execution price, quantity filled, aggressor side, and maker/taker identifiers.
  - **`getCurrentTimestampNs()`**: Computes nanosecond precision timestamps.
  - **`formatIsoTimestamp()`**: Formats the nanosecond timestamps into UTC ISO 8601 strings (e.g. `YYYY-MM-DDTHH:MM:SS.ssssssZ`) required by the specification.
  - **JSON Serialization**: Maps `Trade` objects to JSON strings using the `nlohmann::json` API.

### [src/logger.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/logger.hpp)
* **Purpose**: Implements a thread-safe stdout diagnostics logger.
* **Key Details**:
  - Log levels are prefixed as `LEVEL_DEBUG`, `LEVEL_INFO`, `LEVEL_WARN`, and `LEVEL_ERROR`. This prevents conflicts with the standard Windows header macro `#define ERROR 0`.
  - Employs a singleton instance wrapper accessed via `Logger::getInstance()`.
  - Serializes console outputs using a `std::mutex` guard to avoid interleaved console lines when multiple socket threads log simultaneously.
  - Exposes preprocessor macros (`LOG_INFO`, `LOG_ERROR`, etc.) for easy usage.

---

## 3. Order Book & Matching Core

### [src/order_book.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/order_book.hpp)
* **Purpose**: Header file declaring data mappings and matching engine interfaces.
* **Key Details**:
  - Declares **`PriceLevel`**: Consists of a price coordinate, a running total of active quantity resting at this level, and a double-linked list (`std::list<std::shared_ptr<Order>>`) tracking resting orders in time priority (FIFO).
  - Declares **`OrderBook`**:
    - `bids_`: A `std::map` sorted in descending order (`std::greater<double>`) so that the highest bids are evaluated first.
    - `asks_`: A `std::map` sorted in ascending order (`std::less<double>`) so that the lowest asks are evaluated first.
    - `orders_lookup_`: A hash map (`std::unordered_map<std::string, std::shared_ptr<Order>>`) mapping order IDs to their memory structures for instantaneous access.

### [src/order_book.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/order_book.cpp)
* **Purpose**: Implements the core matching logic.
* **Key Details**:
  - **`submit_order()`**: Evaluates the order type and routes matching to specialized sub-routines:
    - **Limit Order**: Matches crossing prices. Remaining qty is inserted into the book (`add_resting_order()`).
    - **Market Order**: Sweeps the spread immediately. Remaining quantity is discarded (never rests).
    - **IOC (Immediate Or Cancel)**: Matches up to the limit price. Remainder is discarded.
    - **FOK (Fill Or Kill)**: Performs a pre-flight check (`can_fok_be_fully_filled()`). If the book contains sufficient depth to satisfy the entire order at the limit price or better, it executes. Otherwise, it generates zero trades and cancels the FOK order.
  - **`cancel_order()`**: Locates the order in the hash map. Sets its `is_cancelled` flag, and uses the stored `list_it` iterator to erase it from the `PriceLevel` list in `O(1)` time.
  - **`modify_order()`**: If the modification is a size reduction at the same price, the engine updates the size in-place, retaining its FIFO queue position. If the price changes or the size increases, it cancels the old order and submits a new one.
  - **`get_bbo()` / `get_l2_depth()`**: Renders depth levels and BBO calculations as JSON structures.

---

## 4. Manager & Router

### [src/matching_engine.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/matching_engine.hpp) & [matching_engine.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/matching_engine.cpp)
* **Purpose**: Routes incoming orders to the corresponding `OrderBook` instance based on symbol, and implements thread safety.
* **Key Details**:
  - Implements a singleton manager `MatchingEngineManager`.
  - Maintains `books_` (`std::unordered_map<std::string, std::unique_ptr<OrderBook>>`) protected by a `std::recursive_mutex`.
  - Exposes callbacks (`BboCallback`, `DepthCallback`, `TradeCallback`) that are triggered when BBO changes, depth updates, or fills occur.
  - Ensures thread safety across API threads submitting or querying orders.

---

## 5. Web and Networking Layer

### [src/api_server.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/api_server.hpp) & [api_server.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/api_server.cpp)
* **Purpose**: Hosts the REST HTTP API and WebSocket stream services.
* **Key Details**:
  - Integrates HTTP handlers and WebSocket listeners into a single server socket.
  - **`on_http()`**: Handles REST HTTP calls. Matches route names and HTTP methods:
    - `POST /api/orders`: Submits a new order and returns trades instantly in the response body.
    - `DELETE /api/orders`: Cancels a resting order.
    - `GET /api/bbo?symbol=BTC-USDT`: Queries BBO status.
    - `GET /api/depth?symbol=BTC-USDT`: Queries top 10 bids/asks.
  - **`on_open()` / `on_close()`**: Categorizes client WebSocket connections by resource path:
    - `/ws/market-data`: Real-time streaming of BBO and depth.
    - `/ws/trades`: Real-time streaming of trade fills.
    - `/ws/orders`: Bi-directional stream for submitting or cancelling orders over sockets.
  - **`on_message()`**: Parses orders received over `/ws/orders`, submits them to the engine, and sends confirmations.
  - **`broadcast_...()`**: Threads-safely iterates through categorized subscription sets and broadcasts JSON events.

---

## 6. Execution Entry Point

### [src/main.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/src/main.cpp)
* **Purpose**: Coordinates application startup, handles termination signals, and wires callbacks.
* **Key Details**:
  - Hooks `SIGINT` (Ctrl+C) and `SIGTERM` signals.
  - Wires callbacks in `MatchingEngineManager` to invoke `ApiServer` broadcast feeds (hooking trades, BBO, and L2 depth).
  - Launches `ApiServer` on port `8080`.
  - Loops on a sleep timer until terminated, ensuring clean shutdown of the Asio threads.

---

## 7. Testing Invariants

### [tests/test_harness.hpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/tests/test_harness.hpp)
* **Purpose**: A header-only, zero-dependency C++ unit testing framework.
* **Key Details**:
  - Defines macros for registering tests: `TEST(test_name)`.
  - Exposes assertions: `ASSERT_TRUE()`, `ASSERT_FALSE()`, `ASSERT_EQ()`, and `ASSERT_DOUBLE_EQ()`.
  - Automatically registers tests into a static vector for execution.

### [tests/main_test.cpp](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/tests/main_test.cpp)
* **Purpose**: Contains the verification cases.
* **Key Details**:
  - `price_time_priority`: Verifies that overlapping buy limit orders are matched FIFO when crossed by a sell order.
  - `internal_trade_through_protection`: Verifies that an incoming marketable order sweeps the absolute best price first before touching worse price levels.
  - `ioc_order_handling`: Confirms that an IOC matches partially and cancels its remainder immediately.
  - `fok_order_handling`: Confirms that an FOK order dry-runs. It executes if fully satisfiable, and kills itself without executing trades if not.
  - `order_cancellation`: Asserts order is correctly canceled from the book in O(1) and is no longer queryable.
  - `order_modification`: Asserts that decreasing a resting order size preserves its FIFO priority, but changing price causes it to lose priority.

---

## 8. Client Integration & Benchmarks

### [scripts/perf_test.py](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/scripts/perf_test.py)
* **Purpose**: Benchmarks the matching engine by sending high volumes of orders.
* **Key Details**:
  - Uses `asyncio` and `websockets` to connect to `ws://127.0.0.1:8080/ws/orders`.
  - Spawns concurrent tasks: one to stream orders to the server as fast as possible, and another to count and parse matching responses.
  - Displays throughput stats in orders/second.

### [scripts/client_demo.py](file:///c:/Users/Lenovo/Desktop/Crypto_Trading_Engine/scripts/client_demo.py)
* **Purpose**: Performs manual verification of the APIs.
* **Key Details**:
  - Connects to `/ws/market-data` and `/ws/trades` to print updates.
  - Submits resting buys and a crossing sell via HTTP POST to demonstrate real-time feed updates.
