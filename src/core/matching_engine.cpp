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
    if (last_book_ && last_book_->get_symbol() == symbol) {
        return *last_book_;
    }
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        auto book = std::make_unique<OrderBook>(symbol, *clock_, trade_id_seq_, report_seq_, fees_);
        if (auto spec = specs_.find(symbol); spec != specs_.end()) {
            book->set_spec(spec->second);
        }
        last_book_ = book.get();
        books_[symbol] = std::move(book);
        return *last_book_;
    }
    last_book_ = it->second.get();
    return *last_book_;
}

OrderBook* MatchingEngine::find_book(const std::string& symbol) {
    if (last_book_ && last_book_->get_symbol() == symbol) {
        return last_book_;
    }
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        return nullptr;
    }
    last_book_ = it->second.get();
    return last_book_;
}

void MatchingEngine::set_symbol_spec(const SymbolSpec& spec) {
    auto guard = lock();
    specs_[spec.symbol] = spec;
    if (OrderBook* book = find_book(spec.symbol)) {
        book->set_spec(spec);
    }
}

std::optional<SymbolSpec> MatchingEngine::symbol_spec(const std::string& symbol) {
    auto guard = lock();
    auto it = specs_.find(symbol);
    return it == specs_.end() ? std::nullopt : std::optional<SymbolSpec>(it->second);
}

void MatchingEngine::set_default_fee_schedule(const FeeSchedule& schedule) {
    auto guard = lock();
    fees_.set_default(schedule);
}

void MatchingEngine::set_fee_schedule(const std::string& owner, const FeeSchedule& schedule) {
    auto guard = lock();
    fees_.set(owner, schedule);
}

FeeSchedule MatchingEngine::fee_schedule(const std::string& owner) {
    auto guard = lock();
    return fees_.schedule_for(owner);
}

bool MatchingEngine::has_own_fee_schedule(const std::string& owner) {
    auto guard = lock();
    return fees_.own_schedule(owner).has_value();
}

bool MatchingEngine::has_symbol(const std::string& symbol) {
    auto guard = lock();
    return find_book(symbol) != nullptr;
}

void MatchingEngine::deliver(std::vector<ExecReport>& produced, std::vector<ExecReport>* reports) {
    if (exec_report_callback_) {
        for (const auto& r : produced) {
            exec_report_callback_(r);
        }
    }
    if (reports) {
        reports->insert(reports->end(), std::make_move_iterator(produced.begin()),
                        std::make_move_iterator(produced.end()));
    }
}

std::vector<Trade> MatchingEngine::submit_order(std::shared_ptr<Order> order, std::vector<ExecReport>* reports) {
    auto guard = lock();
    auto& book = get_or_create_book(order->symbol);
    begin_call(book, reports);
    Bbo old_bbo = (bbo_callback_ || depth_callback_) ? book.bbo() : Bbo{};
    ReportBuffer produced(report_buffer_);
    std::vector<Trade> trades = book.submit_order(order, produced.reports);
    deliver(produced.reports, reports);
    publish_after_execution(book, *order, old_bbo, trades);
    return trades;
}

std::vector<Trade> MatchingEngine::replace_order(std::shared_ptr<Order> order, std::vector<ExecReport>* reports) {
    auto guard = lock();
    auto& book = get_or_create_book(order->symbol);
    begin_call(book, reports);
    Bbo old_bbo = (bbo_callback_ || depth_callback_) ? book.bbo() : Bbo{};
    ReportBuffer produced(report_buffer_);
    std::vector<Trade> trades = book.replace_order(order, produced.reports);
    deliver(produced.reports, reports);
    publish_after_execution(book, *order, old_bbo, trades);
    return trades;
}

void MatchingEngine::publish_after_execution(OrderBook& book, const Order& order, const Bbo& old_bbo,
                                             const std::vector<Trade>& trades) {
    // Trigger trade callbacks
    if (trade_callback_) {
        for (const auto& trade : trades) {
            trade_callback_(order.symbol, trade);
        }
    }

    // Trigger BBO/depth updates (nothing to compute without listeners, e.g. in a backtest)
    if (!bbo_callback_ && !depth_callback_) {
        return;
    }
    Bbo new_bbo = book.bbo();
    bool bbo_changed = !old_bbo.same_quotes(new_bbo);

    if (bbo_changed) {
        if (bbo_callback_) {
            bbo_callback_(order.symbol, new_bbo);
        }
    }

    // Always trigger L2 depth update on order submission if BBO changed or trade occurred
    if (bbo_changed || !trades.empty() || order.order_type == OrderType::LIMIT) {
        if (depth_callback_) {
            depth_callback_(order.symbol, book.depth(10));
        }
    }
}

bool MatchingEngine::cancel_order(const std::string& symbol, const std::string& order_id,
                                  std::vector<ExecReport>* reports) {
    auto guard = lock();

    OrderBook* found = find_book(symbol);
    if (!found) {
        return false;  // unknown symbol: nothing to cancel or modify (and no book is created)
    }
    auto& book = *found;
    begin_call(book, reports);

    ReportBuffer produced(report_buffer_);
    bool success = book.cancel_order(order_id, produced.reports);
    deliver(produced.reports, reports);

    if (success) {
        if (bbo_callback_) {
            bbo_callback_(symbol, book.bbo());
        }
        if (depth_callback_) {
            depth_callback_(symbol, book.depth(10));
        }
    }

    return success;
}

bool MatchingEngine::modify_order(const std::string& symbol, const std::string& order_id, Quantity new_qty,
                                  Price new_price, std::vector<Trade>& generated_trades,
                                  std::vector<ExecReport>* reports) {
    auto guard = lock();

    OrderBook* found = find_book(symbol);
    if (!found) {
        return false;  // unknown symbol: nothing to cancel or modify (and no book is created)
    }
    auto& book = *found;
    begin_call(book, reports);

    ReportBuffer produced(report_buffer_);
    bool success = book.modify_order(order_id, new_qty, new_price, generated_trades, produced.reports);
    deliver(produced.reports, reports);

    if (success) {
        // Trigger trade callbacks if modification triggered fills
        if (trade_callback_) {
            for (const auto& trade : generated_trades) {
                trade_callback_(symbol, trade);
            }
        }

        if (bbo_callback_) {
            bbo_callback_(symbol, book.bbo());
        }
        if (depth_callback_) {
            depth_callback_(symbol, book.depth(10));
        }
    }

    return success;
}

bool MatchingEngine::amend_order(const std::string& symbol, const std::string& order_id, Quantity new_total,
                                 std::optional<Price> new_price, std::vector<Trade>& generated_trades,
                                 std::vector<ExecReport>* reports) {
    auto guard = lock();

    OrderBook* found = find_book(symbol);
    if (!found) {
        return false;  // unknown symbol: nothing to cancel or modify (and no book is created)
    }
    auto& book = *found;
    begin_call(book, reports);

    ReportBuffer produced(report_buffer_);
    bool success = book.amend_order(order_id, new_total, new_price, generated_trades, produced.reports);
    deliver(produced.reports, reports);

    if (success) {
        // Trigger trade callbacks if modification triggered fills
        if (trade_callback_) {
            for (const auto& trade : generated_trades) {
                trade_callback_(symbol, trade);
            }
        }

        if (bbo_callback_) {
            bbo_callback_(symbol, book.bbo());
        }
        if (depth_callback_) {
            depth_callback_(symbol, book.depth(10));
        }
    }

    return success;
}

Bbo MatchingEngine::bbo(const std::string& symbol) {
    auto guard = lock();
    if (OrderBook* book = find_book(symbol)) {
        book->begin_call(clock_->now_ns(), false);
        return book->bbo();
    }
    Bbo empty;
    empty.timestamp = clock_->now_ns();
    return empty;
}

Depth MatchingEngine::depth(const std::string& symbol, int levels) {
    auto guard = lock();
    if (OrderBook* book = find_book(symbol)) {
        book->begin_call(clock_->now_ns(), false);
        return book->depth(levels);
    }
    Depth empty;
    empty.timestamp = clock_->now_ns();
    return empty;
}

nlohmann::json MatchingEngine::get_bbo(const std::string& symbol) {
    return bbo_to_json(symbol, bbo(symbol));
}

nlohmann::json MatchingEngine::get_l2_depth(const std::string& symbol, int depth_levels) {
    return depth_to_json(symbol, depth(symbol, depth_levels));
}

std::vector<std::string> MatchingEngine::check_invariants(const std::string& symbol) {
    auto guard = lock();
    OrderBook* book = find_book(symbol);
    return book ? book->check_invariants() : std::vector<std::string>{};
}

bool MatchingEngine::is_crossed(const std::string& symbol) {
    auto guard = lock();
    OrderBook* book = find_book(symbol);
    return book && book->is_crossed();
}

void MatchingEngine::reset() {
    auto guard = lock();
    books_.clear();
    last_book_ = nullptr;
    LOG_INFO("All matching engine order books cleared (system reset).");
}
