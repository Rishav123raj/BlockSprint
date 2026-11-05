#pragma once
#include "models.hpp"
#include <string>
#include <vector>

// Plain market-data snapshots (P1.11). The engine works with these on its hot path; JSON is produced
// only at the boundary (API server, getters for tools and tests).

// Best bid and offer. A side without orders has price and quantity 0.
struct Bbo {
    uint64_t timestamp = 0;
    Price bid_price = 0;
    Quantity bid_qty = 0;
    Price ask_price = 0;
    Quantity ask_qty = 0;

    // Same quotes (the timestamp is ignored)
    bool same_quotes(const Bbo& other) const {
        return bid_price == other.bid_price && bid_qty == other.bid_qty && ask_price == other.ask_price &&
               ask_qty == other.ask_qty;
    }
};

struct DepthLevel {
    Price price = 0;
    Quantity quantity = 0;
};

// Aggregated price levels, best first
struct Depth {
    uint64_t timestamp = 0;
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
};

// 1.0 JSON wire format: prices and quantities as "%f" strings, "0.0" for an empty side
inline nlohmann::json bbo_to_json(const std::string& symbol, const Bbo& bbo) {
    nlohmann::json j;
    j["symbol"] = symbol;
    j["timestamp"] = formatIsoTimestamp(bbo.timestamp);
    j["bid_price"] = bbo.bid_qty > 0 ? format_legacy(bbo.bid_price) : "0.0";
    j["bid_qty"] = bbo.bid_qty > 0 ? format_legacy(bbo.bid_qty) : "0.0";
    j["ask_price"] = bbo.ask_qty > 0 ? format_legacy(bbo.ask_price) : "0.0";
    j["ask_qty"] = bbo.ask_qty > 0 ? format_legacy(bbo.ask_qty) : "0.0";
    return j;
}

inline nlohmann::json depth_to_json(const std::string& symbol, const Depth& depth) {
    auto side = [](const std::vector<DepthLevel>& levels) {
        auto arr = nlohmann::json::array();
        for (const auto& level : levels) {
            arr.push_back({format_legacy(level.price), format_legacy(level.quantity)});
        }
        return arr;
    };
    nlohmann::json j;
    j["symbol"] = symbol;
    j["timestamp"] = formatIsoTimestamp(depth.timestamp);
    j["bids"] = side(depth.bids);
    j["asks"] = side(depth.asks);
    return j;
}
