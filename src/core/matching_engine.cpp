#include "matching_engine.hpp"
#include "logger.hpp"

MatchingEngine::MatchingEngine(std::shared_ptr<const IClock> clock, EngineOptions options)
    : clock_(std::move(clock)), options_(options) {
    if (!clock_) {
        throw std::invalid_argument("MatchingEngine requires a clock");
    }
}

std::unique_lock<std::recursive_mutex> MatchingEngine::lock() const {
    std::unique_lock<std::recursive_mutex> guard(mutex_, std::defer_lock);
    if (options_.thread_safe) {
        guard.lock();
    }
    return guard;
}

OrderBook& MatchingEngine::get_or_create_book(const std::string& symbol) {
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        auto book = std::make_unique<OrderBook>(symbol, *clock_, trade_id_seq_);
        auto& ref = *book;
        books_[symbol] = std::move(book);
        return ref;
    }
    return *(it->second);
}

std::vector<Trade> MatchingEngine::submit_order(std::shared_ptr<Order> order) {
    auto guard = lock();
    
    auto& book = get_or_create_book(order->symbol);
    
    // Fetch BBO before execution
    nlohmann::json old_bbo = book.get_bbo();

    // Execute matching
    std::vector<Trade> trades = book.submit_order(order);

    // Trigger trade callbacks
    if (trade_callback_) {
        for (const auto& trade : trades) {
            trade_callback_(order->symbol, trade);
        }
    }

    // Trigger BBO/depth updates
    nlohmann::json new_bbo = book.get_bbo();
    bool bbo_changed = (old_bbo["bid_price"] != new_bbo["bid_price"] ||
                        old_bbo["bid_qty"] != new_bbo["bid_qty"] ||
                        old_bbo["ask_price"] != new_bbo["ask_price"] ||
                        old_bbo["ask_qty"] != new_bbo["ask_qty"]);

    if (bbo_changed) {
        if (bbo_callback_) {
            bbo_callback_(order->symbol, new_bbo);
        }
    }

    // Always trigger L2 depth update on order submission if BBO changed or trade occurred
    if (bbo_changed || !trades.empty() || order->order_type == OrderType::LIMIT) {
        if (depth_callback_) {
            depth_callback_(order->symbol, book.get_l2_depth(10));
        }
    }

    return trades;
}

bool MatchingEngine::cancel_order(const std::string& symbol, const std::string& order_id) {
    auto guard = lock();
    
    auto& book = get_or_create_book(symbol);
    
    nlohmann::json old_bbo = book.get_bbo();
    bool success = book.cancel_order(order_id);

    if (success) {
        nlohmann::json new_bbo = book.get_bbo();
        if (bbo_callback_) {
            bbo_callback_(symbol, new_bbo);
        }
        if (depth_callback_) {
            depth_callback_(symbol, book.get_l2_depth(10));
        }
    }

    return success;
}

bool MatchingEngine::modify_order(const std::string& symbol, const std::string& order_id, Quantity new_qty, Price new_price, std::vector<Trade>& generated_trades) {
    auto guard = lock();
    
    auto& book = get_or_create_book(symbol);
    
    nlohmann::json old_bbo = book.get_bbo();
    bool success = book.modify_order(order_id, new_qty, new_price, generated_trades);

    if (success) {
        // Trigger trade callbacks if modification triggered fills
        if (trade_callback_) {
            for (const auto& trade : generated_trades) {
                trade_callback_(symbol, trade);
            }
        }

        nlohmann::json new_bbo = book.get_bbo();
        if (bbo_callback_) {
            bbo_callback_(symbol, new_bbo);
        }
        if (depth_callback_) {
            depth_callback_(symbol, book.get_l2_depth(10));
        }
    }

    return success;
}

nlohmann::json MatchingEngine::get_bbo(const std::string& symbol) {
    auto guard = lock();
    return get_or_create_book(symbol).get_bbo();
}

nlohmann::json MatchingEngine::get_l2_depth(const std::string& symbol, int depth) {
    auto guard = lock();
    return get_or_create_book(symbol).get_l2_depth(depth);
}

std::vector<std::string> MatchingEngine::check_invariants(const std::string& symbol) {
    auto guard = lock();
    return get_or_create_book(symbol).check_invariants();
}

bool MatchingEngine::is_crossed(const std::string& symbol) {
    auto guard = lock();
    return get_or_create_book(symbol).is_crossed();
}

void MatchingEngine::reset() {
    auto guard = lock();
    books_.clear();
    LOG_INFO("All matching engine order books cleared (system reset).");
}
