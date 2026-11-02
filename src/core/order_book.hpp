#pragma once
#include "clock.hpp"
#include "models.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <list>

struct PriceLevel {
    Price price = 0;
    Quantity total_quantity = 0;
    std::list<std::shared_ptr<Order>> orders;
};

class OrderBook {
public:
    // clock timestamps trades; trade_id_seq is the owning engine's trade ID counter, shared by its books.
    // Both must outlive the book.
    OrderBook(std::string symbol, const IClock& clock, uint64_t& trade_id_seq);
    ~OrderBook() = default;

    // Submits a new order, executes matching, and returns the trades generated.
    std::vector<Trade> submit_order(std::shared_ptr<Order> order);

    // Cancels an order by ID. Returns true if successful.
    bool cancel_order(const std::string& order_id);

    // Modifies an order's quantity and/or price.
    // Retains priority if the quantity is decreased.
    // Loses priority (re-queued at the back) if quantity is increased or price is changed.
    // Returns true if successful.
    bool modify_order(const std::string& order_id, Quantity new_qty, Price new_price, std::vector<Trade>& generated_trades);

    // Returns Best Bid and Best Offer (BBO) as JSON.
    nlohmann::json get_bbo() const;

    // Returns top L2 depth of the book.
    nlohmann::json get_l2_depth(int depth = 10) const;

    const std::string& get_symbol() const { return symbol_; }

    // Consistency check of every internal structure, for tests and debugging. O(orders).
    // Returns one message per violation; empty when the book is consistent.
    std::vector<std::string> check_invariants() const;

    // True if the best bid is at or above the best ask. The 1.0 STP rules can produce this (D7, D21),
    // so it is reported separately from check_invariants() until P1.9.
    bool is_crossed() const;

private:
    std::string symbol_;
    const IClock& clock_;
    uint64_t& trade_id_seq_;
    
    // Bids: Sorted in descending order (highest price first)
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    
    // Asks: Sorted in ascending order (lowest price first)
    std::map<Price, PriceLevel, std::less<Price>> asks_;

    // Order lookup by ID
    std::unordered_map<std::string, std::shared_ptr<Order>> orders_lookup_;

    // Matches the order against the opposite side of the book, best price first.
    // With enforce_limit, stops at the first level the order's price does not cross (LIMIT/IOC/FOK);
    // without it, sweeps until filled or the side is empty (MARKET). Never rests the remainder.
    std::vector<Trade> match_against_book(std::shared_ptr<Order> order, bool enforce_limit);

    template <typename BookSide>
    void sweep(BookSide& book_side, const std::shared_ptr<Order>& order, bool enforce_limit,
               std::vector<Trade>& trades);

    // Dry-run simulation for FOK orders
    bool can_fok_be_fully_filled(std::shared_ptr<Order> order) const;

    template <typename BookSide>
    bool has_fillable_liquidity(const BookSide& book_side, const Order& order) const;

    template <typename BookSide>
    void check_side(const BookSide& book_side, Side side, std::vector<std::string>& violations,
                    size_t& resting_orders) const;

    // Helper to insert a resting order into the book
    void add_resting_order(std::shared_ptr<Order> order);

    // Helper to erase an order from the book data structures
    void remove_order_from_book(std::shared_ptr<Order> order);
};
