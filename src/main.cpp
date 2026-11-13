#include "core/matching_engine.hpp"
#include "api/api_server.hpp"
#include "logger.hpp"
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include <memory>

volatile sig_atomic_t g_running = 1;

void signal_handler(int) {
    g_running = 0;
}

namespace {

void print_usage() {
    std::cout << "Usage: crypto_matching_engine [options]\n"
                 "  --port N           listen on port N (default 8080)\n"
                 "  --feed-queue-model M  /ws/market-feed: the Synthesizer's queue model (default cancel_from_back)\n"
                 "  --feed-seed N         /ws/market-feed: the queue model's seed (default 0)\n"
                 "  --feed-impact-ms T    /ws/market-feed: impact half-life of client taker fills (default 0)\n"
                 "  --log-file PATH    write the log to PATH (appends)\n"
                 "  --log-dir DIR      directory for the per-run log file (default: logs)\n"
                 "  --log-level LEVEL  debug, info (default), warn, error or off\n"
                 "  --no-log           don't write a log file\n"
                 "  --help             show this help\n"
                 "Without --log-file each run writes its own file, DIR/matching_engine-YYYYMMDD-HHMMSS.log (UTC).\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string log_file;
    std::string log_dir = "logs";
    LogLevel log_level = LogLevel::LEVEL_INFO;
    bool no_log = false;
    uint16_t port = 8080;
    sim::LiveFeedOptions feed;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--port" && has_value) {
            const int p = std::atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                std::cerr << "Bad port: " << argv[i] << '\n';
                return 2;
            }
            port = static_cast<uint16_t>(p);
        } else if (arg == "--feed-queue-model" && has_value) {
            feed.queue_model = argv[++i];
        } else if (arg == "--feed-seed" && has_value) {
            feed.seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--feed-impact-ms" && has_value) {
            feed.impact_half_life_ns = static_cast<uint64_t>(std::atof(argv[++i]) * 1e6);
        } else if (arg == "--no-log") {
            no_log = true;
        } else if (arg == "--log-file" && has_value) {
            log_file = argv[++i];
        } else if (arg == "--log-dir" && has_value) {
            log_dir = argv[++i];
        } else if (arg == "--log-level" && has_value) {
            auto level = parse_log_level(argv[++i]);
            if (!level) {
                std::cerr << "Unknown log level: " << argv[i] << '\n';
                return 2;
            }
            log_level = *level;
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << '\n';
            print_usage();
            return 2;
        }
    }

    if (!no_log && log_level != LogLevel::LEVEL_OFF) {
        LoggerOptions options;
        options.path = log_file.empty() ? Logger::run_log_path(log_dir, "matching_engine") : log_file;
        options.level = log_level;
        // The server logs rarely (start/stop, resets), so INFO lines are flushed at once; DEBUG lines
        // (one per WebSocket connection) stay buffered
        options.flush_level = LogLevel::LEVEL_INFO;
        if (Logger::getInstance().open(options)) {
            std::cout << "Logging to " << options.path << " (level " << log_level_name(log_level) << ")\n";
        } else {
            std::cerr << "Warning: can't open log file " << options.path << "; running without a log\n";
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    LOG_INFO("=== Starting Crypto Trading Engine ===");

    MatchingEngine engine;
    // Binance spot regular tier (0.10% maker / 0.10% taker) unless changed through POST /api/fees
    engine.set_default_fee_schedule(binance_spot_regular());
    std::unique_ptr<ApiServer> server_ptr;
    try {
        server_ptr = std::make_unique<ApiServer>(engine, 100, feed);
    } catch (const std::exception& e) {
        std::cerr << "Bad market feed option: " << e.what() << '\n';
        return 2;
    }
    ApiServer& server = *server_ptr;

    // Hook up matching engine events to WebSocket broadcast feeds
    engine.register_bbo_callback(
        [&server](const std::string& symbol, const Bbo& bbo) {
            server.broadcast_bbo(symbol, bbo);
        }
    );

    engine.register_depth_callback(
        [&server](const std::string& symbol, const Depth& depth) {
            server.broadcast_depth(symbol, depth);
        }
    );

    engine.register_trade_callback(
        [&server](const std::string& symbol, const Trade& trade) {
            server.broadcast_trade(symbol, trade);
        }
    );

    engine.register_exec_report_callback(
        [&server](const ExecReport& report) {
            server.broadcast_exec_report(report);
        }
    );

    server.start(port);

    const std::string where = "http://127.0.0.1:" + std::to_string(port) + " and ws://127.0.0.1:" + std::to_string(port);
    LOG_INFO("Crypto Trading Engine is running at " + where + " (market feed: queue model " + feed.queue_model + ")");
    LOG_INFO("Press Ctrl+C to exit.");
    std::cout << "Running at " << where << ". Press Ctrl+C to exit.\n";

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("=== Terminating Crypto Trading Engine ===");
    server.stop();
    LOG_INFO("Server stopped successfully. Goodbye!");
    Logger::getInstance().close();

    return 0;
}
