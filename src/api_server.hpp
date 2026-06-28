#pragma once
#include "models.hpp"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <memory>

typedef websocketpp::server<websocketpp::config::asio> WsServerType;

class ApiServer {
public:
    ApiServer();
    ~ApiServer();

    // Starts the server on the specified port. This method blocks or spawns a thread.
    // We will spawn a background thread for running the server to allow the main thread
    // to do other operations if needed, or to keep it clean.
    void start(uint16_t port);
    
    // Stops the server.
    void stop();

    // Broadcast utilities
    void broadcast_bbo(const std::string& symbol, const nlohmann::json& bbo);
    void broadcast_depth(const std::string& symbol, const nlohmann::json& depth);
    void broadcast_trade(const std::string& symbol, const Trade& trade);

private:
    // Event handlers
    void on_http(websocketpp::connection_hdl hdl);
    void on_open(websocketpp::connection_hdl hdl);
    void on_close(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, WsServerType::message_ptr msg);

    // Helpers to write HTTP responses
    void write_http_response(websocketpp::connection_hdl hdl, 
                             websocketpp::http::status_code::value status, 
                             const std::string& content_type, 
                             const std::string& body);

    WsServerType server_;
    std::unique_ptr<std::thread> server_thread_;
    
    // Subscriber management
    std::mutex connection_mutex_;
    
    // We categorize connections by subscription type
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> market_data_conns_;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> trades_conns_;
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> orders_conns_;
};
