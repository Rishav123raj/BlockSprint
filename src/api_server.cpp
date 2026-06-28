#include "api_server.hpp"
#include "matching_engine.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <exception>

ApiServer::ApiServer() {
    // Suppress WebSocket++ default logging to let our Logger handle diagnostics
    server_.clear_access_channels(websocketpp::log::alevel::all);
    server_.clear_error_channels(websocketpp::log::elevel::all);
}

ApiServer::~ApiServer() {
    stop();
}

void ApiServer::start(uint16_t port) {
    LOG_INFO("Starting API Server on port " + std::to_string(port) + "...");
    
    server_.init_asio();

    // Register Handlers
    server_.set_http_handler([this](websocketpp::connection_hdl hdl) { on_http(hdl); });
    server_.set_open_handler([this](websocketpp::connection_hdl hdl) { on_open(hdl); });
    server_.set_close_handler([this](websocketpp::connection_hdl hdl) { on_close(hdl); });
    server_.set_message_handler([this](websocketpp::connection_hdl hdl, WsServerType::message_ptr msg) { on_message(hdl, msg); });

    server_.listen(port);
    server_.start_accept();

    server_thread_ = std::make_unique<std::thread>([this]() {
        try {
            server_.run();
        } catch (const std::exception& e) {
            LOG_ERROR("API Server run exception: " + std::string(e.what()));
        }
    });
}

void ApiServer::stop() {
    if (server_thread_) {
        LOG_INFO("Stopping API Server...");
        server_.stop_listening();
        server_.stop();
        if (server_thread_->joinable()) {
            server_thread_->join();
        }
        server_thread_.reset();
        LOG_INFO("API Server stopped.");
    }
}

void ApiServer::write_http_response(websocketpp::connection_hdl hdl, 
                                     websocketpp::http::status_code::value status, 
                                     const std::string& content_type, 
                                     const std::string& body) {
    auto con = server_.get_con_from_hdl(hdl);
    con->set_status(status);
    con->replace_header("Content-Type", content_type);
    con->replace_header("Access-Control-Allow-Origin", "*");
    con->replace_header("Access-Control-Allow-Methods", "POST, GET, DELETE, OPTIONS");
    con->replace_header("Access-Control-Allow-Headers", "Content-Type");
    con->set_body(body);
}

void ApiServer::on_http(websocketpp::connection_hdl hdl) {
    auto con = server_.get_con_from_hdl(hdl);
    std::string method = con->get_request().get_method();
    std::string resource = con->get_resource();

    // Pre-flight request support for CORS
    if (method == "OPTIONS") {
        write_http_response(hdl, websocketpp::http::status_code::ok, "text/plain", "");
        return;
    }

    try {
        if (method == "POST" && resource == "/api/orders") {
            auto body = nlohmann::json::parse(con->get_request_body());
            
            // Validate and parse incoming order parameters
            if (!body.contains("symbol") || !body.contains("order_type") || 
                !body.contains("side") || !body.contains("quantity")) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Missing required fields"}};
                write_http_response(hdl, websocketpp::http::status_code::bad_request, "application/json", resp.dump());
                return;
            }

            auto order = std::make_shared<Order>();
            order->order_id = body.contains("order_id") ? body["order_id"].get<std::string>() : std::to_string(getCurrentTimestampNs());
            order->symbol = body["symbol"].get<std::string>();
            order->order_type = stringToOrderType(body["order_type"].get<std::string>());
            order->side = stringToSide(body["side"].get<std::string>());
            order->quantity = std::stod(body["quantity"].get<std::string>());
            order->price = (order->order_type != OrderType::MARKET && body.contains("price")) ? std::stod(body["price"].get<std::string>()) : 0.0;
            order->timestamp = getCurrentTimestampNs();

            std::vector<Trade> trades = MatchingEngineManager::getInstance().submit_order(order);

            nlohmann::json trades_arr = nlohmann::json::array();
            for (const auto& t : trades) {
                trades_arr.push_back(t);
            }

            nlohmann::json resp = {
                {"status", "success"},
                {"order_id", order->order_id},
                {"trades", trades_arr}
            };
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", resp.dump());
            return;
        } 
        
        else if (method == "DELETE" && resource == "/api/orders") {
            auto body = nlohmann::json::parse(con->get_request_body());
            if (!body.contains("symbol") || !body.contains("order_id")) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Missing symbol or order_id"}};
                write_http_response(hdl, websocketpp::http::status_code::bad_request, "application/json", resp.dump());
                return;
            }

            std::string symbol = body["symbol"].get<std::string>();
            std::string order_id = body["order_id"].get<std::string>();

            bool success = MatchingEngineManager::getInstance().cancel_order(symbol, order_id);
            if (success) {
                nlohmann::json resp = {{"status", "success"}, {"message", "Order cancelled"}};
                write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", resp.dump());
            } else {
                nlohmann::json resp = {{"status", "error"}, {"message", "Order not found or already executed"}};
                write_http_response(hdl, websocketpp::http::status_code::not_found, "application/json", resp.dump());
            }
            return;
        } 
        
        else if (method == "GET" && resource.rfind("/api/bbo", 0) == 0) {
            std::string symbol;
            auto query_pos = resource.find("?symbol=");
            if (query_pos != std::string::npos) {
                symbol = resource.substr(query_pos + 8);
            }
            
            if (symbol.empty()) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Missing symbol query parameter"}};
                write_http_response(hdl, websocketpp::http::status_code::bad_request, "application/json", resp.dump());
                return;
            }

            nlohmann::json bbo = MatchingEngineManager::getInstance().get_bbo(symbol);
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", bbo.dump());
            return;
        } 
        
        else if (method == "GET" && resource.rfind("/api/depth", 0) == 0) {
            std::string symbol;
            auto query_pos = resource.find("?symbol=");
            if (query_pos != std::string::npos) {
                symbol = resource.substr(query_pos + 8);
            }

            if (symbol.empty()) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Missing symbol query parameter"}};
                write_http_response(hdl, websocketpp::http::status_code::bad_request, "application/json", resp.dump());
                return;
            }

            nlohmann::json depth = MatchingEngineManager::getInstance().get_l2_depth(symbol, 10);
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", depth.dump());
            return;
        }

        // Not Found
        nlohmann::json resp = {{"status", "error"}, {"message", "Not Found"}};
        write_http_response(hdl, websocketpp::http::status_code::not_found, "application/json", resp.dump());

    } catch (const std::exception& e) {
        nlohmann::json resp = {{"status", "error"}, {"message", e.what()}};
        write_http_response(hdl, websocketpp::http::status_code::bad_request, "application/json", resp.dump());
    }
}

void ApiServer::on_open(websocketpp::connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    auto con = server_.get_con_from_hdl(hdl);
    std::string path = con->get_resource();

    LOG_DEBUG("New WS connection opened on path: " + path);

    if (path.rfind("/ws/market-data", 0) == 0) {
        market_data_conns_.insert(hdl);
    } else if (path.rfind("/ws/trades", 0) == 0) {
        trades_conns_.insert(hdl);
    } else if (path.rfind("/ws/orders", 0) == 0) {
        orders_conns_.insert(hdl);
    }
}

void ApiServer::on_close(websocketpp::connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    market_data_conns_.erase(hdl);
    trades_conns_.erase(hdl);
    orders_conns_.erase(hdl);
    LOG_DEBUG("WS connection closed.");
}

void ApiServer::on_message(websocketpp::connection_hdl hdl, WsServerType::message_ptr msg) {
    auto con = server_.get_con_from_hdl(hdl);
    std::string path = con->get_resource();

    // Only allow incoming order submission/cancel messages on /ws/orders
    if (path.rfind("/ws/orders", 0) == 0) {
        try {
            auto body = nlohmann::json::parse(msg->get_payload());
            std::string action = body.value("action", "submit");

            if (action == "submit") {
                auto order = std::make_shared<Order>();
                order->order_id = body.contains("order_id") ? body["order_id"].get<std::string>() : std::to_string(getCurrentTimestampNs());
                order->symbol = body.at("symbol").get<std::string>();
                order->order_type = stringToOrderType(body.at("order_type").get<std::string>());
                order->side = stringToSide(body.at("side").get<std::string>());
                order->quantity = std::stod(body.at("quantity").get<std::string>());
                order->price = (order->order_type != OrderType::MARKET && body.contains("price")) ? std::stod(body["price"].get<std::string>()) : 0.0;
                order->timestamp = getCurrentTimestampNs();

                std::vector<Trade> trades = MatchingEngineManager::getInstance().submit_order(order);

                nlohmann::json trades_arr = nlohmann::json::array();
                for (const auto& t : trades) {
                    trades_arr.push_back(t);
                }

                nlohmann::json resp = {
                    {"status", "success"},
                    {"order_id", order->order_id},
                    {"trades", trades_arr}
                };
                server_.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
            } 
            else if (action == "cancel") {
                std::string symbol = body.at("symbol").get<std::string>();
                std::string order_id = body.at("order_id").get<std::string>();

                bool success = MatchingEngineManager::getInstance().cancel_order(symbol, order_id);
                nlohmann::json resp = {
                    {"status", success ? "success" : "error"},
                    {"order_id", order_id},
                    {"message", success ? "Order cancelled" : "Order not found or already executed"}
                };
                server_.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
            }
        } catch (const std::exception& e) {
            nlohmann::json resp = {{"status", "error"}, {"message", e.what()}};
            server_.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
        }
    }
}

void ApiServer::broadcast_bbo(const std::string& symbol, const nlohmann::json& bbo) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    
    nlohmann::json msg = {
        {"event", "bbo"},
        {"symbol", symbol},
        {"data", bbo}
    };
    std::string payload = msg.dump();

    for (const auto& hdl : market_data_conns_) {
        websocketpp::lib::error_code ec;
        server_.send(hdl, payload, websocketpp::frame::opcode::text, ec);
    }
}

void ApiServer::broadcast_depth(const std::string& symbol, const nlohmann::json& depth) {
    std::lock_guard<std::mutex> lock(connection_mutex_);

    nlohmann::json msg = {
        {"event", "depth"},
        {"symbol", symbol},
        {"data", depth}
    };
    std::string payload = msg.dump();

    for (const auto& hdl : market_data_conns_) {
        websocketpp::lib::error_code ec;
        server_.send(hdl, payload, websocketpp::frame::opcode::text, ec);
    }
}

void ApiServer::broadcast_trade(const std::string& symbol, const Trade& trade) {
    std::lock_guard<std::mutex> lock(connection_mutex_);

    nlohmann::json trade_json = trade;
    nlohmann::json msg = {
        {"event", "trade"},
        {"symbol", symbol},
        {"data", trade_json}
    };
    std::string payload = msg.dump();

    for (const auto& hdl : trades_conns_) {
        websocketpp::lib::error_code ec;
        server_.send(hdl, payload, websocketpp::frame::opcode::text, ec);
    }
}
