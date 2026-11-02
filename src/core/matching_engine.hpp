#pragma once
#include "clock.hpp"
#include "order_book.hpp"
#include <mutex>
#include <unordered_map>
#include <string>
#include <functional>
#include <memory>

struct EngineOptions {
    // Guard every call with a mutex (live server). A single-threaded simulation turns this off.
    bool thread_safe = true;
};

// Routes orders to per-symbol order books. Each instance is fully independent: its own books,
// its own trade ID sequence (starting at 1) and its own clock, so several engines can run side by
// side, e.g. one per backtest in a parameter sweep.
class MatchingEngine {
public:
    explicit MatchingEngine(std::shared_ptr<const IClock> clock = std::make_shared<WallClock>(),
                            EngineOptions options = {});
    ~MatchingEngine() = default;
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

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
    bool modify_order(const std::string& symbol, const std::string& order_id, Quantity new_qty, Price new_price, std::vector<Trade>& generated_trades);

    // Synchronous state queries
    nlohmann::json get_bbo(const std::string& symbol);
    nlohmann::json get_l2_depth(const std::string& symbol, int depth = 10);

    // Clear all order books (reset state). The trade ID sequence continues, so IDs stay unique.
    void reset();

    // Consistency checks for tests and debugging (see OrderBook::check_invariants)
    std::vector<std::string> check_invariants(const std::string& symbol);
    bool is_crossed(const std::string& symbol);

    const IClock& clock() const { return *clock_; }

private:
    OrderBook& get_or_create_book(const std::string& symbol);

    // Locks the engine mutex when thread_safe is enabled; otherwise returns an unlocked guard
    std::unique_lock<std::recursive_mutex> lock() const;

    std::shared_ptr<const IClock> clock_;
    EngineOptions options_;
    mutable std::recursive_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;

    // Last trade ID issued by any book of this engine
    uint64_t trade_id_seq_ = 0;

    BboCallback bbo_callback_;
    DepthCallback depth_callback_;
    TradeCallback trade_callback_;
};
