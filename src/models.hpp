#pragma once
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>

enum class OrderType {
    LIMIT,
    MARKET,
    IOC,
    FOK
};

enum class Side {
    BUY,
    SELL
};

inline std::string orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::LIMIT: return "limit";
        case OrderType::MARKET: return "market";
        case OrderType::IOC: return "ioc";
        case OrderType::FOK: return "fok";
    }
    return "unknown";
}

inline OrderType stringToOrderType(const std::string& str) {
    if (str == "limit") return OrderType::LIMIT;
    if (str == "market") return OrderType::MARKET;
    if (str == "ioc") return OrderType::IOC;
    if (str == "fok") return OrderType::FOK;
    throw std::invalid_argument("Invalid order type: " + str);
}

inline std::string sideToString(Side side) {
    switch (side) {
        case Side::BUY: return "buy";
        case Side::SELL: return "sell";
    }
    return "unknown";
}

inline Side stringToSide(const std::string& str) {
    if (str == "buy") return Side::BUY;
    if (str == "sell") return Side::SELL;
    throw std::invalid_argument("Invalid side: " + str);
}

// Forward declaration
struct Order;

struct Order {
    std::string order_id;
    std::string symbol;
    OrderType order_type;
    Side side;
    double price; // Only relevant for limit/ioc/fok
    double quantity;
    double remaining_quantity;
    uint64_t timestamp;
    bool is_cancelled = false;

    // Iterator pointing to the position of this order in its PriceLevel's std::list.
    // This allows O(1) removals.
    std::list<std::shared_ptr<Order>>::iterator list_it;
    bool has_iterator = false;
};

struct Trade {
    std::string trade_id;
    std::string symbol;
    double price;
    double quantity;
    Side aggressor_side;
    std::string maker_order_id;
    std::string taker_order_id;
    uint64_t timestamp;
};

inline uint64_t getCurrentTimestampNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

inline std::string formatIsoTimestamp(uint64_t timestamp_ns) {
    auto secs = timestamp_ns / 1000000000ULL;
    auto micros = (timestamp_ns % 1000000000ULL) / 1000ULL;
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm_val;
    
    // Windows safe gmtime
#if defined(_WIN32) || defined(_WIN64)
    gmtime_s(&tm_val, &t);
#else
    gmtime_r(&t, &tm_val);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%S")
       << "." << std::setw(6) << std::setfill('0') << micros
       << "Z";
    return ss.str();
}

// JSON Serialization macros/functions
inline void to_json(nlohmann::json& j, const Trade& t) {
    j = nlohmann::json{
        {"timestamp", formatIsoTimestamp(t.timestamp)},
        {"symbol", t.symbol},
        {"trade_id", t.trade_id},
        {"price", std::to_string(t.price)},
        {"quantity", std::to_string(t.quantity)},
        {"aggressor_side", sideToString(t.aggressor_side)},
        {"maker_order_id", t.maker_order_id},
        {"taker_order_id", t.taker_order_id}
    };
}
