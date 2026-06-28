# File: src/main.cpp

This is the main entry point of the project. It coordinates initialization, registers callback delegates, launches the WebSocket/HTTP server, and gracefully handles termination signals from the operating system.

---

## What it Does

1. **Catches Termination Signals**: Registers a signal handler for `SIGINT` (Ctrl+C) and `SIGTERM`. When triggered, it flips the volatile state `g_running` flag to `0`, prompting a graceful shutdown.
2. **Wires Up Event Broadcasting**: Connects the `MatchingEngineManager` callbacks to the `ApiServer` broadcast mechanisms using lambda closures:
   - When a BBO changes, it invokes `ApiServer::broadcast_bbo`.
   - When L2 depth updates, it invokes `ApiServer::broadcast_depth`.
   - When a trade matches, it invokes `ApiServer::broadcast_trade`.
3. **Launches Networking Loops**: Starts the unified HTTP/WebSocket `ApiServer` on port `8080`.
4. **Sleep Loop**: Keeps the main thread alive using a sleep loop (`std::this_thread::sleep_for(100ms)`) checking the `g_running` flag.
5. **Graceful Cleanup**: Once terminated, it shuts down listening sockets, stops the Asio thread loop, joins the network worker threads, and releases all resources before exiting.

---

## Architectural Diagram

The diagram below shows the main thread execution and signals setup:

```mermaid
sequenceDiagram
    participant OS as Operating System
    participant Main as main() Thread
    participant MEM as MatchingEngineManager
    participant Server as ApiServer

    Main->>Main: Register SIGINT/SIGTERM handlers
    Main->>MEM: Register BboCallback lambda -> invokes Server.broadcast_bbo
    Main->>MEM: Register DepthCallback lambda -> invokes Server.broadcast_depth
    Main->>MEM: Register TradeCallback lambda -> invokes Server.broadcast_trade
    
    Main->>Server: Start listening on Port 8080
    Server->>Server: Spawns Network Loop Thread
    
    Note over Main: Enters sleep loop while (g_running)
    
    OS->>Main: Sends SIGINT (Ctrl + C)
    Main->>Main: Signal handler sets g_running = 0
    Main->>Main: Breaks sleep loop
    
    Main->>Server: Stop Server
    Server->>Server: Stop listening, close connections, join thread
    Main->>Main: Exit program successfully (0)
```
