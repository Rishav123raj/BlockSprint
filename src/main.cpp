#include "matching_engine.hpp"
#include "api_server.hpp"
#include "logger.hpp"
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>

volatile sig_atomic_t g_running = 1;

void signal_handler(int) {
    g_running = 0;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    LOG_INFO("=== Starting Crypto Trading Engine ===");

    ApiServer server;

    // Hook up matching engine events to WebSocket broadcast feeds
    MatchingEngineManager::getInstance().register_bbo_callback(
        [&server](const std::string& symbol, const nlohmann::json& bbo) {
            server.broadcast_bbo(symbol, bbo);
        }
    );

    MatchingEngineManager::getInstance().register_depth_callback(
        [&server](const std::string& symbol, const nlohmann::json& depth) {
            server.broadcast_depth(symbol, depth);
        }
    );

    MatchingEngineManager::getInstance().register_trade_callback(
        [&server](const std::string& symbol, const Trade& trade) {
            server.broadcast_trade(symbol, trade);
        }
    );

    // Start the REST/WebSocket API Server on Port 8080
    server.start(8080);
    
    LOG_INFO("Crypto Trading Engine is running at http://127.0.0.1:8080 and ws://127.0.0.1:8080");
    LOG_INFO("Press Ctrl+C to exit.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("=== Terminating Crypto Trading Engine ===");
    server.stop();
    LOG_INFO("Server stopped successfully. Goodbye!");

    return 0;
}
