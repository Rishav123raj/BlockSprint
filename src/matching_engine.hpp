#pragma once
#include "order_book.hpp"
#include <mutex>
#include <unordered_map>
#include <string>
#include <functional>
#include <memory>

class MatchingEngineManager {
public:
    static MatchingEngineManager& getInstance() {
        static MatchingEngineManager instance;
        return instance;
    }

    using BboCallback = std::function<void(const std::string& symbol, const nlohmann::json& bbo)>;
    using DepthCallback = std::function<void(const std::string& symbol, const nlohmann::json& depth)>;
    using TradeCallback = std::function<void(const std::string& symbol, const Trade& trade)>;

    void register_bbo_callback(BboCallback cb) { bbo_callback_ = std::move(cb); }
    void register_depth_callback(DepthCallback cb) { depth_callback_ = std::move(cb); }
    void register_trade_callback(TradeCallback cb) { trade_callback_ = std::move(cb); }

    // Submits an order and triggers matching
    std::vector<Trade> submit_order(std::shared_ptr<Order> order);

    // Cancels a resting order
    bool cancel_order(const std::string& symbol, const std::string& order_id);

    // Modifies a resting order
    bool modify_order(const std::string& symbol, const std::string& order_id, double new_qty, double new_price, std::vector<Trade>& generated_trades);

    // Synchronous state queries
    nlohmann::json get_bbo(const std::string& symbol);
    nlohmann::json get_l2_depth(const std::string& symbol, int depth = 10);

private:
    MatchingEngineManager() = default;
    ~MatchingEngineManager() = default;
    MatchingEngineManager(const MatchingEngineManager&) = delete;
    MatchingEngineManager& operator=(const MatchingEngineManager&) = delete;

    OrderBook& get_or_create_book(const std::string& symbol);

    mutable std::recursive_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;

    BboCallback bbo_callback_;
    DepthCallback depth_callback_;
    TradeCallback trade_callback_;
};
