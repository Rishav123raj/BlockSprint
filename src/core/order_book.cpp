#include "order_book.hpp"
#include <algorithm>

OrderBook::OrderBook(std::string symbol, const IClock& clock, uint64_t& trade_id_seq)
    : symbol_(std::move(symbol)), clock_(clock), trade_id_seq_(trade_id_seq) {}

std::vector<Trade> OrderBook::submit_order(std::shared_ptr<Order> order) {
    if (orders_lookup_.find(order->order_id) != orders_lookup_.end()) {
        cancel_order(order->order_id);
    }

    order->remaining_quantity = order->quantity;
    order->is_cancelled = false;
    order->has_iterator = false;

    std::vector<Trade> trades;

    switch (order->order_type) {
        case OrderType::LIMIT:
            trades = match_against_book(order, true);
            if (order->remaining_quantity > 0) {
                add_resting_order(order);
            }
            break;
        case OrderType::MARKET:
            trades = match_against_book(order, false);
            break;
        case OrderType::IOC:
            trades = match_against_book(order, true);
            break;
        case OrderType::FOK:
            // Only execute once the dry run confirms a complete fill. As in 1.0, a remainder can still
            // rest when self-trade makers stop the sweep early (v1-quirk "D21"); fixed with STP rework in P1.9.
            if (can_fok_be_fully_filled(order)) {
                trades = match_against_book(order, true);
                if (order->remaining_quantity > 0) {
                    add_resting_order(order);
                }
            }
            break;
    }

    return trades;
}

bool OrderBook::cancel_order(const std::string& order_id) {
    auto it = orders_lookup_.find(order_id);
    if (it == orders_lookup_.end()) {
        return false;
    }

    auto order = it->second;
    order->is_cancelled = true;
    remove_order_from_book(order);
    orders_lookup_.erase(it);
    return true;
}

bool OrderBook::modify_order(const std::string& order_id, Quantity new_qty, Price new_price, std::vector<Trade>& generated_trades) {
    auto it = orders_lookup_.find(order_id);
    if (it == orders_lookup_.end()) {
        return false;
    }

    auto order = it->second;

    // Standard exchange rules:
    // 1. If price is unchanged and new qty is less than current remaining quantity, we keep priority.
    if (order->price == new_price && new_qty < order->remaining_quantity) {
        Quantity diff = order->remaining_quantity - new_qty;
        order->remaining_quantity = new_qty;
        
        if (order->side == Side::BUY) {
            auto price_it = bids_.find(order->price);
            if (price_it != bids_.end()) {
                price_it->second.total_quantity -= diff;
            }
        } else {
            auto price_it = asks_.find(order->price);
            if (price_it != asks_.end()) {
                price_it->second.total_quantity -= diff;
            }
        }

        if (new_qty == 0) {
            cancel_order(order_id);
        }
        return true;
    }

    // 2. Otherwise (price change or qty increase), order is cancelled and a new one is submitted.
    // Retain original order details
    std::string symbol = order->symbol;
    Side side = order->side;
    OrderType type = order->order_type;

    // Cancel old order
    cancel_order(order_id);

    // Resubmit new order
    auto new_order = std::make_shared<Order>();
    new_order->order_id = order_id;
    new_order->symbol = symbol;
    new_order->order_type = type;
    new_order->side = side;
    new_order->price = new_price;
    new_order->quantity = new_qty;
    new_order->timestamp = clock_.now_ns();

    generated_trades = submit_order(new_order);
    return true;
}

nlohmann::json OrderBook::get_bbo() const {
    nlohmann::json j;
    j["symbol"] = symbol_;
    j["timestamp"] = formatIsoTimestamp(clock_.now_ns());

    if (!bids_.empty()) {
        auto best_bid = bids_.begin();
        j["bid_price"] = format_legacy(best_bid->first);
        j["bid_qty"] = format_legacy(best_bid->second.total_quantity);
    } else {
        j["bid_price"] = "0.0";
        j["bid_qty"] = "0.0";
    }

    if (!asks_.empty()) {
        auto best_ask = asks_.begin();
        j["ask_price"] = format_legacy(best_ask->first);
        j["ask_qty"] = format_legacy(best_ask->second.total_quantity);
    } else {
        j["ask_price"] = "0.0";
        j["ask_qty"] = "0.0";
    }

    return j;
}

nlohmann::json OrderBook::get_l2_depth(int depth) const {
    nlohmann::json j;
    j["symbol"] = symbol_;
    j["timestamp"] = formatIsoTimestamp(clock_.now_ns());

    auto bids_arr = nlohmann::json::array();
    int b_count = 0;
    for (const auto& [price, level] : bids_) {
        if (b_count >= depth) break;
        bids_arr.push_back({format_legacy(price), format_legacy(level.total_quantity)});
        b_count++;
    }
    j["bids"] = bids_arr;

    auto asks_arr = nlohmann::json::array();
    int a_count = 0;
    for (const auto& [price, level] : asks_) {
        if (a_count >= depth) break;
        asks_arr.push_back({format_legacy(price), format_legacy(level.total_quantity)});
        a_count++;
    }
    j["asks"] = asks_arr;

    return j;
}

namespace {

// True if an order at this price may trade with a resting level at level_price.
bool crosses(const Order& order, Price level_price) {
    return order.side == Side::BUY ? order.price >= level_price : order.price <= level_price;
}

// Self-trade prevention: mirrored Binance liquidity never matches other mirrored Binance liquidity.
bool is_self_trade(const Order& taker, const Order& maker) {
    return taker.order_id.rfind("BINANCE_", 0) == 0 && maker.order_id.rfind("BINANCE_", 0) == 0;
}

} // namespace

template <typename BookSide>
void OrderBook::sweep(BookSide& book_side, const std::shared_ptr<Order>& order, bool enforce_limit,
                      std::vector<Trade>& trades) {
    while (!book_side.empty() && order->remaining_quantity > 0) {
        auto level_it = book_side.begin();
        if (enforce_limit && !crosses(*order, level_it->first)) {
            break;
        }

        PriceLevel& level = level_it->second;
        auto order_it = level.orders.begin();
        while (order_it != level.orders.end() && order->remaining_quantity > 0) {
            auto maker_order = *order_it;
            if (is_self_trade(*order, *maker_order)) {
                ++order_it;
                continue;
            }

            Quantity match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
            uint64_t trade_id = ++trade_id_seq_;

            trades.push_back(Trade{
                std::to_string(trade_id),
                symbol_,
                maker_order->price,
                match_qty,
                order->side,
                maker_order->order_id,
                order->order_id,
                clock_.now_ns()
            });

            maker_order->remaining_quantity -= match_qty;
            order->remaining_quantity -= match_qty;
            level.total_quantity -= match_qty;

            if (maker_order->remaining_quantity <= 0) {
                orders_lookup_.erase(maker_order->order_id);
                order_it = level.orders.erase(order_it);
            } else {
                ++order_it;
            }
        }

        // A level that still has orders either filled the taker or holds only self-trade makers;
        // in both cases stop here (1.0 behavior, see the D7 characterization test).
        if (level.orders.empty()) {
            book_side.erase(level_it);
        } else {
            break;
        }
    }
}

std::vector<Trade> OrderBook::match_against_book(std::shared_ptr<Order> order, bool enforce_limit) {
    std::vector<Trade> trades;
    if (order->side == Side::BUY) {
        sweep(asks_, order, enforce_limit, trades);
    } else {
        sweep(bids_, order, enforce_limit, trades);
    }
    return trades;
}

template <typename BookSide>
bool OrderBook::has_fillable_liquidity(const BookSide& book_side, const Order& order) const {
    Quantity accumulated_qty = 0;
    for (const auto& [price, level] : book_side) {
        if (!crosses(order, price)) {
            break;
        }
        for (const auto& resting_order : level.orders) {
            if (is_self_trade(order, *resting_order)) {
                continue;
            }
            accumulated_qty += resting_order->remaining_quantity;
            if (accumulated_qty >= order.quantity) {
                return true;
            }
        }
    }
    return false;
}

bool OrderBook::can_fok_be_fully_filled(std::shared_ptr<Order> order) const {
    if (order->side == Side::BUY) {
        return has_fillable_liquidity(asks_, *order);
    }
    return has_fillable_liquidity(bids_, *order);
}

void OrderBook::add_resting_order(std::shared_ptr<Order> order) {
    orders_lookup_[order->order_id] = order;

    if (order->side == Side::BUY) {
        PriceLevel& level = bids_[order->price];
        level.price = order->price;
        level.total_quantity += order->remaining_quantity;
        
        level.orders.push_back(order);
        order->list_it = std::prev(level.orders.end());
        order->has_iterator = true;
    } else {
        PriceLevel& level = asks_[order->price];
        level.price = order->price;
        level.total_quantity += order->remaining_quantity;

        level.orders.push_back(order);
        order->list_it = std::prev(level.orders.end());
        order->has_iterator = true;
    }
}

void OrderBook::remove_order_from_book(std::shared_ptr<Order> order) {
    if (!order->has_iterator) return;

    if (order->side == Side::BUY) {
        auto price_it = bids_.find(order->price);
        if (price_it != bids_.end()) {
            PriceLevel& level = price_it->second;
            level.total_quantity -= order->remaining_quantity;
            level.orders.erase(order->list_it);
            
            if (level.orders.empty()) {
                bids_.erase(price_it);
            }
        }
    } else {
        auto price_it = asks_.find(order->price);
        if (price_it != asks_.end()) {
            PriceLevel& level = price_it->second;
            level.total_quantity -= order->remaining_quantity;
            level.orders.erase(order->list_it);

            if (level.orders.empty()) {
                asks_.erase(price_it);
            }
        }
    }
    order->has_iterator = false;
}

template <typename BookSide>
void OrderBook::check_side(const BookSide& book_side, Side side, std::vector<std::string>& violations,
                           size_t& resting_orders) const {
    const std::string side_name = sideToString(side);
    for (const auto& [price, level] : book_side) {
        const std::string where = side_name + " level " + format_fixed(price);
        if (level.orders.empty()) {
            violations.push_back(where + ": empty level was not erased");
        }
        if (level.price != price) {
            violations.push_back(where + ": PriceLevel.price is " + format_fixed(level.price));
        }

        Quantity sum = 0;
        for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
            const auto& order = *it;
            const std::string who = where + ", order " + order->order_id;
            ++resting_orders;
            sum += order->remaining_quantity;

            if (order->side != side) {
                violations.push_back(who + ": rests on the wrong side");
            }
            if (order->price != price) {
                violations.push_back(who + ": price " + format_fixed(order->price) + " differs from its level");
            }
            if (order->remaining_quantity <= 0) {
                violations.push_back(who + ": non-positive remaining quantity");
            }
            if (order->is_cancelled) {
                violations.push_back(who + ": cancelled order still queued");
            }
            if (!order->has_iterator || order->list_it != it) {
                violations.push_back(who + ": list back-pointer does not point at its queue position");
            }
            auto found = orders_lookup_.find(order->order_id);
            if (found == orders_lookup_.end() || found->second != order) {
                violations.push_back(who + ": missing from the order lookup");
            }
        }

        if (sum != level.total_quantity) {
            violations.push_back(where + ": total_quantity " + format_fixed(level.total_quantity) +
                                 " != sum of orders " + format_fixed(sum));
        }
    }
}

std::vector<std::string> OrderBook::check_invariants() const {
    std::vector<std::string> violations;
    size_t resting_orders = 0;
    check_side(bids_, Side::BUY, violations, resting_orders);
    check_side(asks_, Side::SELL, violations, resting_orders);

    if (resting_orders != orders_lookup_.size()) {
        violations.push_back("order lookup holds " + std::to_string(orders_lookup_.size()) +
                             " orders but the queues hold " + std::to_string(resting_orders));
    }
    return violations;
}

bool OrderBook::is_crossed() const {
    return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first;
}
