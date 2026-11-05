#pragma once
#include "core/matching_engine.hpp"
#include "core/market_data.hpp"
#include "core/models.hpp"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <asio/steady_timer.hpp>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <memory>

typedef websocketpp::server<websocketpp::config::asio> WsServerType;

class ApiServer {
public:
    // The engine must outlive the server.
    // Depth updates are coalesced per symbol and sent at most every depth_interval_ms (always the latest
    // book, like Binance's @depth@100ms streams); 0 sends every update immediately (1.0 behavior).
    explicit ApiServer(MatchingEngine& engine, int depth_interval_ms = 100);
    ~ApiServer();

    // Starts the server on the specified port. This method blocks or spawns a thread.
    // We will spawn a background thread for running the server to allow the main thread
    // to do other operations if needed, or to keep it clean.
    void start(uint16_t port);
    
    // Stops the server.
    void stop();

    // Broadcast utilities
    void broadcast_bbo(const std::string& symbol, const Bbo& bbo);
    void broadcast_depth(const std::string& symbol, const Depth& depth);
    void broadcast_trade(const std::string& symbol, const Trade& trade);
    void broadcast_exec_report(const ExecReport& report);

private:
    // Event handlers
    void on_http(websocketpp::connection_hdl hdl);
    void on_open(websocketpp::connection_hdl hdl);
    void on_close(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, WsServerType::message_ptr msg);

    // Owner reserved for mirrored exchange liquidity; accepted only from loopback connections
    inline static const std::string kMarketOwner = "MARKET";

    // Builds an order from a REST/WS request body (validates fields and the owner)
    std::shared_ptr<Order> parse_order(const nlohmann::json& body, websocketpp::connection_hdl hdl);

    // {"status": "success", "order_id": ..., "trades": [...], "reports": [...]}; status "error" with the
    // report's message if the order was rejected
    static nlohmann::json order_response(const Order& order, const std::vector<Trade>& trades,
                                         const std::vector<ExecReport>& reports);

    // Amends an order from a REST/WS body {symbol, order_id, quantity (new total), price?}; sets found
    // (the order was resting) and was_rejected (the amend broke a rule)
    nlohmann::json amend(const nlohmann::json& body, bool& found, bool& was_rejected);

    // True if the last report is a REJECTED report (the request's order was refused)
    static bool rejected(const std::vector<ExecReport>& reports);

    // True if the connection comes from this machine
    bool is_loopback(websocketpp::connection_hdl hdl);

    // Sends a depth update to market-data subscribers now
    void send_depth(const std::string& symbol, const Depth& depth);

    // Depth coalescing: the timer flushes the latest pending depth of every changed symbol
    void schedule_depth_flush();
    void flush_depth();

    // 404 {"status": "error", "message": "Unknown symbol '...'"}
    void write_unknown_symbol(websocketpp::connection_hdl hdl, const std::string& symbol);

    // Helpers to write HTTP responses
    void write_http_response(websocketpp::connection_hdl hdl, 
                             websocketpp::http::status_code::value status, 
                             const std::string& content_type, 
                             const std::string& body);

    MatchingEngine& engine_;
    int depth_interval_ms_;
    WsServerType server_;
    std::unique_ptr<asio::steady_timer> depth_timer_;
    std::mutex depth_mutex_;
    std::map<std::string, Depth> pending_depth_;
    std::unique_ptr<std::thread> server_thread_;
    
    // Subscriber management
    std::mutex connection_mutex_;
    
    // We categorize connections by subscription type
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> market_data_conns_;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> trades_conns_;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> orders_conns_;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> executions_conns_;
};
