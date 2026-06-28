#pragma once
#include "models.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <list>

struct PriceLevel {
    double price;
    double total_quantity = 0.0;
    std::list<std::shared_ptr<Order>> orders;
};

class OrderBook {
public:
    explicit OrderBook(std::string symbol);
    ~OrderBook() = default;

    // Submits a new order, executes matching, and returns the trades generated.
    std::vector<Trade> submit_order(std::shared_ptr<Order> order);

    // Cancels an order by ID. Returns true if successful.
    bool cancel_order(const std::string& order_id);

    // Modifies an order's quantity and/or price.
    // Retains priority if the quantity is decreased.
    // Loses priority (re-queued at the back) if quantity is increased or price is changed.
    // Returns true if successful.
    bool modify_order(const std::string& order_id, double new_qty, double new_price, std::vector<Trade>& generated_trades);

    // Returns Best Bid and Best Offer (BBO) as JSON.
    nlohmann::json get_bbo() const;

    // Returns top L2 depth of the book.
    nlohmann::json get_l2_depth(int depth = 10) const;

    const std::string& get_symbol() const { return symbol_; }

private:
    std::string symbol_;
    
    // Bids: Sorted in descending order (highest price first)
    std::map<double, PriceLevel, std::greater<double>> bids_;
    
    // Asks: Sorted in ascending order (lowest price first)
    std::map<double, PriceLevel, std::less<double>> asks_;

    // Order lookup by ID
    std::unordered_map<std::string, std::shared_ptr<Order>> orders_lookup_;

    // Match execution helpers
    std::vector<Trade> match_limit_order(std::shared_ptr<Order> order);
    std::vector<Trade> match_market_order(std::shared_ptr<Order> order);
    std::vector<Trade> match_ioc_order(std::shared_ptr<Order> order);
    std::vector<Trade> match_fok_order(std::shared_ptr<Order> order);

    // Dry-run simulation for FOK orders
    bool can_fok_be_fully_filled(std::shared_ptr<Order> order) const;

    // Helper to insert a resting order into the book
    void add_resting_order(std::shared_ptr<Order> order);

    // Helper to erase an order from the book data structures
    void remove_order_from_book(std::shared_ptr<Order> order);
};
