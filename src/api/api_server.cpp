#include "api_server.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <exception>
#include <stdexcept>

namespace {

// Exact decimal parse of a price or quantity field given as a JSON string ("0.5") or number (0.5).
// Numbers are parsed from their shortest JSON text, so 0.1 stays exactly 0.1.
int64_t parse_decimal_field(const nlohmann::json& value, const char* field) {
    std::string text;
    if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_number()) {
        text = value.dump();
    } else {
        throw std::invalid_argument(std::string("Invalid ") + field + ": expected a decimal string or number");
    }
    auto parsed = parse_fixed(text);
    if (!parsed) {
        throw std::invalid_argument(std::string("Invalid ") + field + " '" + text +
                                    "': expected a non-negative decimal with at most 8 decimal places");
    }
    return *parsed;
}

// Optional decimal field of a symbol spec; 0 (no rule) when absent
int64_t optional_decimal(const nlohmann::json& body, const char* field) {
    return body.contains(field) ? parse_decimal_field(body[field], field) : 0;
}

nlohmann::json spec_to_json(const SymbolSpec& spec) {
    return {
        {"symbol", spec.symbol},
        {"tick_size", format_fixed(spec.tick_size)},
        {"lot_size", format_fixed(spec.lot_size)},
        {"min_qty", format_fixed(spec.min_qty)},
        {"max_qty", format_fixed(spec.max_qty)},
        {"min_notional", format_fixed(spec.min_notional)},
    };
}

// Value of `?symbol=` in a resource path, or "" if absent
std::string symbol_query(const std::string& resource) {
    auto query_pos = resource.find("?symbol=");
    return query_pos == std::string::npos ? "" : resource.substr(query_pos + 8);
}

// Signed fee rate as a decimal fraction: "0.001" (0.10%) or "-0.00005" (a 0.005% rebate)
int64_t parse_rate(const nlohmann::json& value, const char* field) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (!text.empty() && text[0] == '-') {
            return -parse_decimal_field(nlohmann::json(text.substr(1)), field);
        }
    } else if (value.is_number() && value.get<double>() < 0) {
        std::string text = value.dump();
        return -parse_decimal_field(nlohmann::json(text.substr(1)), field);
    }
    return parse_decimal_field(value, field);
}

nlohmann::json fees_to_json(const std::string& owner, const FeeSchedule& schedule, bool own) {
    return {
        {"owner", owner},
        {"maker_rate", format_fixed(schedule.maker_rate)},
        {"taker_rate", format_fixed(schedule.taker_rate)},
        {"source", own ? "owner" : "default"},
    };
}

} // namespace

ApiServer::ApiServer(MatchingEngine& engine, int depth_interval_ms)
    : engine_(engine), depth_interval_ms_(depth_interval_ms) {
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

    if (depth_interval_ms_ > 0) {
        depth_timer_ = std::make_unique<asio::steady_timer>(server_.get_io_service());
        schedule_depth_flush();
    }

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
        depth_timer_.reset();  // its pending wait was abandoned when the io_service stopped
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
    con->replace_header("Access-Control-Allow-Methods", "POST, PUT, PATCH, GET, DELETE, OPTIONS");
    con->replace_header("Access-Control-Allow-Headers", "Content-Type");
    con->set_body(body);
}

bool ApiServer::is_loopback(websocketpp::connection_hdl hdl) {
    auto con = server_.get_con_from_hdl(hdl);
    asio::error_code ec;
    auto address = con->get_raw_socket().remote_endpoint(ec).address();
    if (ec) {
        return false;
    }
    if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
    }
    return address.is_loopback();
}

std::shared_ptr<Order> ApiServer::parse_order(const nlohmann::json& body, websocketpp::connection_hdl hdl) {
    if (!body.contains("symbol") || !body.contains("order_type") || !body.contains("side") ||
        !body.contains("quantity")) {
        throw std::invalid_argument("Missing required fields: symbol, order_type, side and quantity are required");
    }

    auto order = std::make_shared<Order>();
    order->order_id = body.contains("order_id") ? body["order_id"].get<std::string>() : std::to_string(engine_.clock().now_ns());
    order->symbol = body["symbol"].get<std::string>();
    order->order_type = stringToOrderType(body["order_type"].get<std::string>());
    order->side = stringToSide(body["side"].get<std::string>());
    order->quantity = parse_decimal_field(body["quantity"], "quantity");
    // A missing price on LIMIT / IOC / FOK stays 0 and the engine rejects it (invalid_price)
    order->price = (order->order_type != OrderType::MARKET && body.contains("price")) ? parse_decimal_field(body["price"], "price") : 0;
    order->timestamp = engine_.clock().now_ns();

    // Self-trade prevention: optional participant and policy (default cancel_maker, as on Binance)
    if (body.contains("owner")) {
        order->owner_id = body["owner"].get<std::string>();
        if (order->owner_id == kMarketOwner) {
            if (!is_loopback(hdl)) {
                throw std::invalid_argument("owner '" + kMarketOwner + "' is reserved for local market-data connectors");
            }
            // Mirrored exchange levels are aggregates, not participant orders: exempt from symbol rules
            order->market_data_liquidity = true;
        }
    }
    if (body.contains("stp")) {
        order->stp_policy = stringToStpPolicy(body["stp"].get<std::string>());
    }
    return order;
}

nlohmann::json ApiServer::amend(const nlohmann::json& body, bool& found, bool& was_rejected) {
    if (!body.contains("symbol") || !body.contains("order_id") || !body.contains("quantity")) {
        throw std::invalid_argument("Missing required fields: symbol, order_id and quantity (the new total) are required");
    }
    std::string symbol = body["symbol"].get<std::string>();
    std::string order_id = body["order_id"].get<std::string>();
    Quantity new_total = parse_decimal_field(body["quantity"], "quantity");
    std::optional<Price> new_price;
    if (body.contains("price")) {
        new_price = parse_decimal_field(body["price"], "price");
    }

    std::vector<ExecReport> reports;
    std::vector<Trade> trades;
    bool ok = engine_.amend_order(symbol, order_id, new_total, new_price, trades, &reports);
    was_rejected = rejected(reports);
    found = ok || was_rejected;

    nlohmann::json trades_arr = nlohmann::json::array();
    for (const auto& t : trades) {
        trades_arr.push_back(t);
    }
    nlohmann::json resp = {{"status", ok ? "success" : "error"}, {"order_id", order_id}, {"trades", trades_arr},
                           {"reports", reports}};
    if (was_rejected) {
        resp["message"] = reports.back().text;
    } else if (!found) {
        resp["message"] = "Order not found or already executed";
    }
    return resp;
}

nlohmann::json ApiServer::order_response(const Order& order, const std::vector<Trade>& trades,
                                         const std::vector<ExecReport>& reports) {
    nlohmann::json trades_arr = nlohmann::json::array();
    for (const auto& t : trades) {
        trades_arr.push_back(t);
    }
    nlohmann::json resp = {
        {"status", "success"},
        {"order_id", order.order_id},
        {"trades", trades_arr},
        {"reports", reports}
    };
    if (!reports.empty() && reports.back().type == ExecType::REJECTED) {
        resp["status"] = "error";
        resp["message"] = reports.back().text;
    }
    return resp;
}

bool ApiServer::rejected(const std::vector<ExecReport>& reports) {
    return !reports.empty() && reports.back().type == ExecType::REJECTED;
}

void ApiServer::write_unknown_symbol(websocketpp::connection_hdl hdl, const std::string& symbol) {
    nlohmann::json resp = {{"status", "error"}, {"message", "Unknown symbol '" + symbol + "'"}};
    write_http_response(hdl, websocketpp::http::status_code::not_found, "application/json", resp.dump());
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
        if ((method == "POST" || method == "PUT") && resource == "/api/orders") {
            // POST submits a new order; PUT replaces the resting order with the same order_id
            auto body = nlohmann::json::parse(con->get_request_body());
            auto order = parse_order(body, hdl);
            std::vector<ExecReport> reports;
            std::vector<Trade> trades =
                method == "POST" ? engine_.submit_order(order, &reports) : engine_.replace_order(order, &reports);
            write_http_response(hdl,
                                rejected(reports) ? websocketpp::http::status_code::bad_request
                                                  : websocketpp::http::status_code::ok,
                                "application/json", order_response(*order, trades, reports).dump());
            return;
        }

        else if (method == "PATCH" && resource == "/api/orders") {
            // Amend: new TOTAL quantity (filled + open, as on Binance) and optionally a new price
            auto body = nlohmann::json::parse(con->get_request_body());
            bool found = false;
            bool was_rejected = false;
            nlohmann::json resp = amend(body, found, was_rejected);
            write_http_response(hdl,
                                !found         ? websocketpp::http::status_code::not_found
                                : was_rejected ? websocketpp::http::status_code::bad_request
                                               : websocketpp::http::status_code::ok,
                                "application/json", resp.dump());
            return;
        }

        else if (method == "POST" && resource == "/api/reset") {
            engine_.reset();
            nlohmann::json resp = {{"status", "success"}, {"message", "Matching engine state reset successfully"}};
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

            std::vector<ExecReport> reports;
            bool success = engine_.cancel_order(symbol, order_id, &reports);
            if (success) {
                nlohmann::json resp = {{"status", "success"}, {"message", "Order cancelled"}, {"reports", reports}};
                write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", resp.dump());
            } else {
                nlohmann::json resp = {{"status", "error"}, {"message", "Order not found or already executed"}};
                write_http_response(hdl, websocketpp::http::status_code::not_found, "application/json", resp.dump());
            }
            return;
        } 
        
        else if (method == "GET" && resource.rfind("/api/bbo", 0) == 0) {
            std::string symbol = symbol_query(resource);
            if (symbol.empty()) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Missing symbol query parameter"}};
                write_http_response(hdl, websocketpp::http::status_code::bad_request, "application/json", resp.dump());
                return;
            }
            if (!engine_.has_symbol(symbol)) {
                write_unknown_symbol(hdl, symbol);
                return;
            }

            nlohmann::json bbo = engine_.get_bbo(symbol);
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", bbo.dump());
            return;
        } 
        
        else if (method == "GET" && resource.rfind("/api/depth", 0) == 0) {
            std::string symbol = symbol_query(resource);
            if (symbol.empty()) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Missing symbol query parameter"}};
                write_http_response(hdl, websocketpp::http::status_code::bad_request, "application/json", resp.dump());
                return;
            }
            if (!engine_.has_symbol(symbol)) {
                write_unknown_symbol(hdl, symbol);
                return;
            }

            nlohmann::json depth = engine_.get_l2_depth(symbol, 10);
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", depth.dump());
            return;
        }

        else if (method == "POST" && resource == "/api/symbols") {
            // Trading rules (Binance filters) for a symbol; only local connectors may set them
            if (!is_loopback(hdl)) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Symbol rules can only be set from this machine"}};
                write_http_response(hdl, websocketpp::http::status_code::forbidden, "application/json", resp.dump());
                return;
            }
            auto body = nlohmann::json::parse(con->get_request_body());
            if (!body.contains("symbol")) {
                throw std::invalid_argument("Missing symbol");
            }
            SymbolSpec spec;
            spec.symbol = body["symbol"].get<std::string>();
            spec.tick_size = optional_decimal(body, "tick_size");
            spec.lot_size = optional_decimal(body, "lot_size");
            spec.min_qty = optional_decimal(body, "min_qty");
            spec.max_qty = optional_decimal(body, "max_qty");
            spec.min_notional = optional_decimal(body, "min_notional");
            engine_.set_symbol_spec(spec);
            nlohmann::json resp = {{"status", "success"}, {"spec", spec_to_json(spec)}};
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", resp.dump());
            return;
        }

        else if (method == "POST" && resource == "/api/fees") {
            // Fee schedule for one owner, or the default when "owner" is absent; only from this machine
            if (!is_loopback(hdl)) {
                nlohmann::json resp = {{"status", "error"}, {"message", "Fees can only be set from this machine"}};
                write_http_response(hdl, websocketpp::http::status_code::forbidden, "application/json", resp.dump());
                return;
            }
            auto body = nlohmann::json::parse(con->get_request_body());
            if (!body.contains("maker_rate") || !body.contains("taker_rate")) {
                throw std::invalid_argument("Missing maker_rate or taker_rate (decimal fractions, e.g. \"0.001\")");
            }
            FeeSchedule schedule{parse_rate(body["maker_rate"], "maker_rate"), parse_rate(body["taker_rate"], "taker_rate")};
            std::string owner = body.value("owner", "");
            if (owner.empty()) {
                engine_.set_default_fee_schedule(schedule);
            } else {
                engine_.set_fee_schedule(owner, schedule);
            }
            nlohmann::json resp = {{"status", "success"}, {"fees", fees_to_json(owner, schedule, !owner.empty())}};
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", resp.dump());
            return;
        }

        else if (method == "GET" && resource.rfind("/api/fees", 0) == 0) {
            // The schedule that applies to ?owner=... (the default if omitted or unknown)
            auto pos = resource.find("?owner=");
            std::string owner = pos == std::string::npos ? "" : resource.substr(pos + 7);
            FeeSchedule schedule = engine_.fee_schedule(owner);
            bool own = !owner.empty() && engine_.has_own_fee_schedule(owner);
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json",
                                fees_to_json(owner, schedule, own).dump());
            return;
        }

        else if (method == "GET" && resource.rfind("/api/symbols", 0) == 0) {
            std::string symbol = symbol_query(resource);
            auto spec = engine_.symbol_spec(symbol);
            if (!spec) {
                nlohmann::json resp = {{"status", "error"}, {"message", "No trading rules set for symbol '" + symbol + "'"}};
                write_http_response(hdl, websocketpp::http::status_code::not_found, "application/json", resp.dump());
                return;
            }
            write_http_response(hdl, websocketpp::http::status_code::ok, "application/json", spec_to_json(*spec).dump());
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
    } else if (path.rfind("/ws/executions", 0) == 0) {
        executions_conns_.insert(hdl);
    }
}

void ApiServer::on_close(websocketpp::connection_hdl hdl) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    market_data_conns_.erase(hdl);
    trades_conns_.erase(hdl);
    orders_conns_.erase(hdl);
    executions_conns_.erase(hdl);
    LOG_DEBUG("WS connection closed.");
}

void ApiServer::on_message(websocketpp::connection_hdl hdl, WsServerType::message_ptr msg) {
    auto con = server_.get_con_from_hdl(hdl);
    std::string path = con->get_resource();

    // Only allow incoming order submission/cancel messages on /ws/orders
    if (path.rfind("/ws/orders", 0) == 0) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(msg->get_payload());
            std::string action = body.value("action", "submit");

            if (action == "submit" || action == "replace") {
                auto order = parse_order(body, hdl);
                std::vector<ExecReport> reports;
                std::vector<Trade> trades = action == "submit" ? engine_.submit_order(order, &reports)
                                                               : engine_.replace_order(order, &reports);
                server_.send(hdl, order_response(*order, trades, reports).dump(), websocketpp::frame::opcode::text);
            }
            else if (action == "amend") {
                bool found = false;
                bool was_rejected = false;
                server_.send(hdl, amend(body, found, was_rejected).dump(), websocketpp::frame::opcode::text);
            }
            else if (action == "cancel") {
                std::string symbol = body.at("symbol").get<std::string>();
                std::string order_id = body.at("order_id").get<std::string>();

                std::vector<ExecReport> reports;
                bool success = engine_.cancel_order(symbol, order_id, &reports);
                nlohmann::json resp = {
                    {"status", success ? "success" : "error"},
                    {"order_id", order_id},
                    {"message", success ? "Order cancelled" : "Order not found or already executed"},
                    {"reports", reports}
                };
                server_.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
            }
        } catch (const std::exception& e) {
            nlohmann::json resp = {{"status", "error"}, {"message", e.what()}};
            if (body.is_object() && body.contains("order_id") && body["order_id"].is_string()) {
                resp["order_id"] = body["order_id"];
            }
            server_.send(hdl, resp.dump(), websocketpp::frame::opcode::text);
        }
    }
}

void ApiServer::broadcast_bbo(const std::string& symbol, const Bbo& bbo) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (market_data_conns_.empty()) {
        return;  // nobody to send to: skip building the JSON
    }

    nlohmann::json msg = {
        {"event", "bbo"},
        {"symbol", symbol},
        {"data", bbo_to_json(symbol, bbo)}
    };
    std::string payload = msg.dump();

    for (const auto& hdl : market_data_conns_) {
        websocketpp::lib::error_code ec;
        server_.send(hdl, payload, websocketpp::frame::opcode::text, ec);
    }
}

void ApiServer::broadcast_depth(const std::string& symbol, const Depth& depth) {
    if (depth_interval_ms_ <= 0) {
        send_depth(symbol, depth);
        return;
    }
    std::lock_guard<std::mutex> lock(depth_mutex_);
    pending_depth_[symbol] = depth;  // only the latest book per symbol is kept
}

void ApiServer::send_depth(const std::string& symbol, const Depth& depth) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (market_data_conns_.empty()) {
        return;
    }

    nlohmann::json msg = {
        {"event", "depth"},
        {"symbol", symbol},
        {"data", depth_to_json(symbol, depth)}
    };
    std::string payload = msg.dump();

    for (const auto& hdl : market_data_conns_) {
        websocketpp::lib::error_code ec;
        server_.send(hdl, payload, websocketpp::frame::opcode::text, ec);
    }
}

void ApiServer::schedule_depth_flush() {
    depth_timer_->expires_after(std::chrono::milliseconds(depth_interval_ms_));
    depth_timer_->async_wait([this](const asio::error_code& ec) {
        if (ec) {
            return;  // cancelled: the server is stopping
        }
        flush_depth();
        schedule_depth_flush();
    });
}

void ApiServer::flush_depth() {
    std::map<std::string, Depth> ready;
    {
        std::lock_guard<std::mutex> lock(depth_mutex_);
        ready.swap(pending_depth_);
    }
    for (const auto& [symbol, depth] : ready) {
        send_depth(symbol, depth);
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

void ApiServer::broadcast_exec_report(const ExecReport& report) {
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (executions_conns_.empty()) {
        return;
    }

    nlohmann::json msg = {
        {"event", "execution"},
        {"symbol", report.symbol},
        {"data", report}
    };
    std::string payload = msg.dump();

    for (const auto& hdl : executions_conns_) {
        websocketpp::lib::error_code ec;
        server_.send(hdl, payload, websocketpp::frame::opcode::text, ec);
    }
}
