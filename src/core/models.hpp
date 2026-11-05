#pragma once
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include "types.hpp"

enum class OrderType {
    LIMIT,
    MARKET,
    IOC,
    FOK,
    LIMIT_MAKER  // post-only: rests on the book, or is rejected if it would trade on arrival (P1.14)
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
        case OrderType::LIMIT_MAKER: return "limit_maker";
    }
    return "unknown";
}

inline OrderType stringToOrderType(const std::string& str) {
    if (str == "limit") return OrderType::LIMIT;
    if (str == "market") return OrderType::MARKET;
    if (str == "ioc") return OrderType::IOC;
    if (str == "fok") return OrderType::FOK;
    if (str == "limit_maker" || str == "post_only") return OrderType::LIMIT_MAKER;
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

// Self-trade prevention: what happens when an incoming order would trade with a resting order of the
// same owner. The incoming (taker) order's policy decides. Orders without an owner never self-trade.
enum class StpPolicy {
    NONE,          // allow the self-trade
    SKIP,          // pass over own resting orders and keep matching others (used for mirrored liquidity)
    CANCEL_TAKER,  // stop matching and cancel the rest of the incoming order (Binance EXPIRE_TAKER)
    CANCEL_MAKER,  // cancel the own resting order and keep matching (Binance EXPIRE_MAKER)
    CANCEL_BOTH    // cancel the own resting order and the rest of the incoming order (Binance EXPIRE_BOTH)
};

inline std::string stpPolicyToString(StpPolicy policy) {
    switch (policy) {
        case StpPolicy::NONE: return "none";
        case StpPolicy::SKIP: return "skip";
        case StpPolicy::CANCEL_TAKER: return "cancel_taker";
        case StpPolicy::CANCEL_MAKER: return "cancel_maker";
        case StpPolicy::CANCEL_BOTH: return "cancel_both";
    }
    return "unknown";
}

inline StpPolicy stringToStpPolicy(const std::string& str) {
    if (str == "none") return StpPolicy::NONE;
    if (str == "skip") return StpPolicy::SKIP;
    if (str == "cancel_taker") return StpPolicy::CANCEL_TAKER;
    if (str == "cancel_maker") return StpPolicy::CANCEL_MAKER;
    if (str == "cancel_both") return StpPolicy::CANCEL_BOTH;
    throw std::invalid_argument("Invalid stp policy: " + str);
}

// Forward declaration
struct Order;

struct Order {
    std::string order_id;
    std::string symbol;
    OrderType order_type;
    Side side;
    Price price = 0; // Only relevant for limit/ioc/fok
    Quantity quantity = 0;
    Quantity remaining_quantity = 0;
    Quantity filled_quantity = 0;  // cumulative fills, kept across modify
    int64_t fees_paid = 0;         // cumulative fees in quote units (1e-8; negative = rebates), kept across modify
    uint64_t timestamp;
    bool is_cancelled = false;

    // Participant that owns the order (e.g. "MARKET" for mirrored exchange liquidity). Empty = anonymous.
    std::string owner_id;
    // Applied when this order is the incoming order. CANCEL_MAKER matches Binance's default.
    StpPolicy stp_policy = StpPolicy::CANCEL_MAKER;
    // Liquidity reconstructed from market data (a mirrored or replayed exchange price level) rather than a
    // participant's order. Exempt from the symbol's trading rules: real books hold levels below the
    // minimum notional, for example. The basic checks (positive quantity and price) still apply.
    bool market_data_liquidity = false;

    // Iterator pointing to the position of this order in its PriceLevel's std::list.
    // This allows O(1) removals.
    std::list<std::shared_ptr<Order>>::iterator list_it;
    bool has_iterator = false;
};

struct Trade {
    std::string trade_id;
    std::string symbol;
    Price price;
    Quantity quantity;
    Side aggressor_side;
    std::string maker_order_id;
    std::string taker_order_id;
    uint64_t timestamp;
};

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
        {"price", format_legacy(t.price)},
        {"quantity", format_legacy(t.quantity)},
        {"aggressor_side", sideToString(t.aggressor_side)},
        {"maker_order_id", t.maker_order_id},
        {"taker_order_id", t.taker_order_id}
    };
}
