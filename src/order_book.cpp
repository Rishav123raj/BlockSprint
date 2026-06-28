#include "order_book.hpp"
#include <atomic>
#include <iostream>
#include <algorithm>

// Thread-safe trade counter for unique trade IDs
static std::atomic<uint64_t> g_trade_id_counter{0};

OrderBook::OrderBook(std::string symbol) : symbol_(std::move(symbol)) {}

std::vector<Trade> OrderBook::submit_order(std::shared_ptr<Order> order) {
    order->remaining_quantity = order->quantity;
    order->is_cancelled = false;
    order->has_iterator = false;

    std::vector<Trade> trades;

    switch (order->order_type) {
        case OrderType::LIMIT:
            trades = match_limit_order(order);
            break;
        case OrderType::MARKET:
            trades = match_market_order(order);
            break;
        case OrderType::IOC:
            trades = match_ioc_order(order);
            break;
        case OrderType::FOK:
            trades = match_fok_order(order);
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

bool OrderBook::modify_order(const std::string& order_id, double new_qty, double new_price, std::vector<Trade>& generated_trades) {
    auto it = orders_lookup_.find(order_id);
    if (it == orders_lookup_.end()) {
        return false;
    }

    auto order = it->second;

    // Standard exchange rules:
    // 1. If price is unchanged and new qty is less than current remaining quantity, we keep priority.
    if (order->price == new_price && new_qty < order->remaining_quantity) {
        double diff = order->remaining_quantity - new_qty;
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
    new_order->timestamp = getCurrentTimestampNs();

    generated_trades = submit_order(new_order);
    return true;
}

nlohmann::json OrderBook::get_bbo() const {
    nlohmann::json j;
    j["symbol"] = symbol_;
    j["timestamp"] = formatIsoTimestamp(getCurrentTimestampNs());

    if (!bids_.empty()) {
        auto best_bid = bids_.begin();
        j["bid_price"] = std::to_string(best_bid->first);
        j["bid_qty"] = std::to_string(best_bid->second.total_quantity);
    } else {
        j["bid_price"] = "0.0";
        j["bid_qty"] = "0.0";
    }

    if (!asks_.empty()) {
        auto best_ask = asks_.begin();
        j["ask_price"] = std::to_string(best_ask->first);
        j["ask_qty"] = std::to_string(best_ask->second.total_quantity);
    } else {
        j["ask_price"] = "0.0";
        j["ask_qty"] = "0.0";
    }

    return j;
}

nlohmann::json OrderBook::get_l2_depth(int depth) const {
    nlohmann::json j;
    j["symbol"] = symbol_;
    j["timestamp"] = formatIsoTimestamp(getCurrentTimestampNs());

    auto bids_arr = nlohmann::json::array();
    int b_count = 0;
    for (const auto& [price, level] : bids_) {
        if (b_count >= depth) break;
        bids_arr.push_back({std::to_string(price), std::to_string(level.total_quantity)});
        b_count++;
    }
    j["bids"] = bids_arr;

    auto asks_arr = nlohmann::json::array();
    int a_count = 0;
    for (const auto& [price, level] : asks_) {
        if (a_count >= depth) break;
        asks_arr.push_back({std::to_string(price), std::to_string(level.total_quantity)});
        a_count++;
    }
    j["asks"] = asks_arr;

    return j;
}

std::vector<Trade> OrderBook::match_limit_order(std::shared_ptr<Order> order) {
    std::vector<Trade> trades;

    if (order->side == Side::BUY) {
        while (!asks_.empty() && order->remaining_quantity > 0) {
            auto ask_level_it = asks_.begin();
            double best_ask_price = ask_level_it->first;
            
            // Limit price check
            if (order->price < best_ask_price) {
                break;
            }

            PriceLevel& level = ask_level_it->second;
            auto order_it = level.orders.begin();
            while (order_it != level.orders.end() && order->remaining_quantity > 0) {
                auto maker_order = *order_it;
                if (maker_order->is_cancelled) {
                    order_it = level.orders.erase(order_it);
                    continue;
                }

                double match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
                uint64_t trade_id = ++g_trade_id_counter;

                Trade trade{
                    std::to_string(trade_id),
                    symbol_,
                    maker_order->price,
                    match_qty,
                    Side::BUY,
                    maker_order->order_id,
                    order->order_id,
                    getCurrentTimestampNs()
                };
                trades.push_back(trade);

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

            if (level.orders.empty()) {
                asks_.erase(ask_level_it);
            }
        }

        if (order->remaining_quantity > 0) {
            add_resting_order(order);
        }
    } else { // SELL Side
        while (!bids_.empty() && order->remaining_quantity > 0) {
            auto bid_level_it = bids_.begin();
            double best_bid_price = bid_level_it->first;

            // Limit price check
            if (order->price > best_bid_price) {
                break;
            }

            PriceLevel& level = bid_level_it->second;
            auto order_it = level.orders.begin();
            while (order_it != level.orders.end() && order->remaining_quantity > 0) {
                auto maker_order = *order_it;
                if (maker_order->is_cancelled) {
                    order_it = level.orders.erase(order_it);
                    continue;
                }

                double match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
                uint64_t trade_id = ++g_trade_id_counter;

                Trade trade{
                    std::to_string(trade_id),
                    symbol_,
                    maker_order->price,
                    match_qty,
                    Side::SELL,
                    maker_order->order_id,
                    order->order_id,
                    getCurrentTimestampNs()
                };
                trades.push_back(trade);

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

            if (level.orders.empty()) {
                bids_.erase(bid_level_it);
            }
        }

        if (order->remaining_quantity > 0) {
            add_resting_order(order);
        }
    }

    return trades;
}

std::vector<Trade> OrderBook::match_market_order(std::shared_ptr<Order> order) {
    std::vector<Trade> trades;

    if (order->side == Side::BUY) {
        while (!asks_.empty() && order->remaining_quantity > 0) {
            auto ask_level_it = asks_.begin();
            PriceLevel& level = ask_level_it->second;
            auto order_it = level.orders.begin();
            while (order_it != level.orders.end() && order->remaining_quantity > 0) {
                auto maker_order = *order_it;
                if (maker_order->is_cancelled) {
                    order_it = level.orders.erase(order_it);
                    continue;
                }

                double match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
                uint64_t trade_id = ++g_trade_id_counter;

                Trade trade{
                    std::to_string(trade_id),
                    symbol_,
                    maker_order->price,
                    match_qty,
                    Side::BUY,
                    maker_order->order_id,
                    order->order_id,
                    getCurrentTimestampNs()
                };
                trades.push_back(trade);

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

            if (level.orders.empty()) {
                asks_.erase(ask_level_it);
            }
        }
    } else { // SELL Side
        while (!bids_.empty() && order->remaining_quantity > 0) {
            auto bid_level_it = bids_.begin();
            PriceLevel& level = bid_level_it->second;
            auto order_it = level.orders.begin();
            while (order_it != level.orders.end() && order->remaining_quantity > 0) {
                auto maker_order = *order_it;
                if (maker_order->is_cancelled) {
                    order_it = level.orders.erase(order_it);
                    continue;
                }

                double match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
                uint64_t trade_id = ++g_trade_id_counter;

                Trade trade{
                    std::to_string(trade_id),
                    symbol_,
                    maker_order->price,
                    match_qty,
                    Side::SELL,
                    maker_order->order_id,
                    order->order_id,
                    getCurrentTimestampNs()
                };
                trades.push_back(trade);

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

            if (level.orders.empty()) {
                bids_.erase(bid_level_it);
            }
        }
    }

    return trades;
}

std::vector<Trade> OrderBook::match_ioc_order(std::shared_ptr<Order> order) {
    std::vector<Trade> trades;

    if (order->side == Side::BUY) {
        while (!asks_.empty() && order->remaining_quantity > 0) {
            auto ask_level_it = asks_.begin();
            double best_ask_price = ask_level_it->first;

            if (order->price < best_ask_price) {
                break;
            }

            PriceLevel& level = ask_level_it->second;
            auto order_it = level.orders.begin();
            while (order_it != level.orders.end() && order->remaining_quantity > 0) {
                auto maker_order = *order_it;
                if (maker_order->is_cancelled) {
                    order_it = level.orders.erase(order_it);
                    continue;
                }

                double match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
                uint64_t trade_id = ++g_trade_id_counter;

                Trade trade{
                    std::to_string(trade_id),
                    symbol_,
                    maker_order->price,
                    match_qty,
                    Side::BUY,
                    maker_order->order_id,
                    order->order_id,
                    getCurrentTimestampNs()
                };
                trades.push_back(trade);

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

            if (level.orders.empty()) {
                asks_.erase(ask_level_it);
            }
        }
    } else { // SELL Side
        while (!bids_.empty() && order->remaining_quantity > 0) {
            auto bid_level_it = bids_.begin();
            double best_bid_price = bid_level_it->first;

            if (order->price > best_bid_price) {
                break;
            }

            PriceLevel& level = bid_level_it->second;
            auto order_it = level.orders.begin();
            while (order_it != level.orders.end() && order->remaining_quantity > 0) {
                auto maker_order = *order_it;
                if (maker_order->is_cancelled) {
                    order_it = level.orders.erase(order_it);
                    continue;
                }

                double match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
                uint64_t trade_id = ++g_trade_id_counter;

                Trade trade{
                    std::to_string(trade_id),
                    symbol_,
                    maker_order->price,
                    match_qty,
                    Side::SELL,
                    maker_order->order_id,
                    order->order_id,
                    getCurrentTimestampNs()
                };
                trades.push_back(trade);

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

            if (level.orders.empty()) {
                bids_.erase(bid_level_it);
            }
        }
    }

    return trades;
}

std::vector<Trade> OrderBook::match_fok_order(std::shared_ptr<Order> order) {
    if (!can_fok_be_fully_filled(order)) {
        return {}; // Cancel the entire order (no match)
    }

    // Since we verified that it can be fully filled, we can reuse limit order matching.
    return match_limit_order(order);
}

bool OrderBook::can_fok_be_fully_filled(std::shared_ptr<Order> order) const {
    double needed_qty = order->quantity;
    double accumulated_qty = 0.0;

    if (order->side == Side::BUY) {
        for (const auto& [price, level] : asks_) {
            if (order->price < price) {
                break;
            }

            for (const auto& resting_order : level.orders) {
                if (resting_order->is_cancelled) continue;
                accumulated_qty += resting_order->remaining_quantity;
                if (accumulated_qty >= needed_qty) {
                    return true;
                }
            }
        }
    } else { // SELL Side
        for (const auto& [price, level] : bids_) {
            if (order->price > price) {
                break;
            }

            for (const auto& resting_order : level.orders) {
                if (resting_order->is_cancelled) continue;
                accumulated_qty += resting_order->remaining_quantity;
                if (accumulated_qty >= needed_qty) {
                    return true;
                }
            }
        }
    }

    return false;
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
