#pragma once
#include "clock.hpp"
#include "events.hpp"
#include "fees.hpp"
#include "market_data.hpp"
#include "models.hpp"
#include "symbol_spec.hpp"
#include <map>
#include <optional>
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

// Every mutating call appends the execution reports it produces to `reports` (see events.hpp).
class OrderBook {
public:
    // clock timestamps trades and reports; trade_id_seq and report_seq are the owning engine's
    // counters and fees its fee schedules, shared by its books. All must outlive the book.
    OrderBook(std::string symbol, const IClock& clock, uint64_t& trade_id_seq, uint64_t& report_seq,
              const FeeBook& fees);
    ~OrderBook() = default;

    // Submits a new order, executes matching, and returns the trades generated.
    // Rejected (REJECTED report, no trades) if an order with the same ID is resting or the order breaks
    // a rule (validate_order: quantity, price, and the symbol spec if one is set).
    // A LIMIT remainder rests only if it does not cross the book; otherwise it is cancelled (WOULD_CROSS).
    // That can only happen when self-trade prevention skipped resting orders.
    std::vector<Trade> submit_order(std::shared_ptr<Order> order, std::vector<ExecReport>& reports);

    // Cancels the resting order with the same ID (if any, reason REPLACED), then submits the new one.
    // The new order joins the back of its price level's queue.
    std::vector<Trade> replace_order(std::shared_ptr<Order> order, std::vector<ExecReport>& reports);

    // Cancels an order by ID. Returns true if successful.
    bool cancel_order(const std::string& order_id, std::vector<ExecReport>& reports);

    // Amends an order to a new TOTAL quantity (filled + open, as on Binance) and optionally a new price
    // (P1.14). Keeps queue priority only when the total is reduced at the same price; an increase or a
    // price change re-queues it. A total at or below the filled quantity cancels the rest (USER).
    // Rejected with the order unchanged if the new values break the symbol's rules, or if a post-only
    // order would trade at the new price. Returns false if not resting or rejected.
    bool amend_order(const std::string& order_id, Quantity new_total, std::optional<Price> new_price,
                     std::vector<Trade>& generated_trades, std::vector<ExecReport>& reports);

    // Modifies an order's OPEN quantity and/or price (the 1.0 semantics; amend_order is the Binance-style
    // call). Reported as AMENDED; zero quantity cancels.
    // A new price or quantity that breaks the symbol's rules is REJECTED and the order is left unchanged.
    // Retains priority if the quantity is decreased at the same price.
    // Loses priority (re-queued at the back) if quantity is increased or price is changed.
    // Returns true if successful.
    bool modify_order(const std::string& order_id, Quantity new_qty, Price new_price,
                      std::vector<Trade>& generated_trades, std::vector<ExecReport>& reports);

    // Best Bid and Best Offer, timestamped by the book's clock
    Bbo bbo() const;

    // Top `levels` aggregated price levels per side, best first
    Depth depth(int levels = 10) const;

    const std::string& get_symbol() const { return symbol_; }

    // Trading rules applied to new orders and modifications from now on
    void set_spec(const SymbolSpec& spec) { spec_ = spec; }
    const SymbolSpec* spec() const { return spec_ ? &*spec_ : nullptr; }

    // Consistency check of every internal structure, for tests and debugging. O(orders).
    // Returns one message per violation (including a crossed book); empty when the book is consistent.
    std::vector<std::string> check_invariants() const;

    // True if the best bid is at or above the best ask. Never true since P1.9 (it was in 1.0: D7, D21).
    bool is_crossed() const;

private:
    std::string symbol_;
    const IClock& clock_;
    uint64_t& trade_id_seq_;
    uint64_t& report_seq_;
    const FeeBook& fees_;
    std::optional<SymbolSpec> spec_;

    // Bids: Sorted in descending order (highest price first)
    std::map<Price, PriceLevel, std::greater<Price>> bids_;

    // Asks: Sorted in ascending order (lowest price first)
    std::map<Price, PriceLevel, std::less<Price>> asks_;

    // Order lookup by ID
    std::unordered_map<std::string, std::shared_ptr<Order>> orders_lookup_;

    // Validates, reports first_report (ACK, or AMENDED for a modify), matches and rests or cancels the remainder
    std::vector<Trade> submit_internal(std::shared_ptr<Order> order, std::vector<ExecReport>& reports,
                                       ExecType first_report);

    // Rejection message for a post-only order that would trade
    std::string post_only_text(const Order& order) const;

    // Removes a resting order without reporting it. Returns the order, or nullptr if not found.
    std::shared_ptr<Order> erase_order(const std::string& order_id);

    // Appends a report describing the order's current state and returns it for further fields
    ExecReport& report(std::vector<ExecReport>& reports, ExecType type, const Order& order,
                       ExecReason reason = ExecReason::NONE);

    // Matches the order against the opposite side of the book, best price first.
    // With enforce_limit, stops at the first level the order's price does not cross (LIMIT/IOC/FOK);
    // without it, sweeps until filled or the side is empty (MARKET). Never rests the remainder.
    // taker_cancelled is set when self-trade prevention cancelled the rest of the incoming order.
    std::vector<Trade> match_against_book(std::shared_ptr<Order> order, bool enforce_limit, bool& taker_cancelled,
                                          std::vector<ExecReport>& reports);

    template <typename BookSide>
    void sweep(BookSide& book_side, const std::shared_ptr<Order>& order, bool enforce_limit,
               std::vector<Trade>& trades, bool& taker_cancelled, std::vector<ExecReport>& reports);

    // True if a resting order at this price would cross the opposite side
    bool would_cross(const Order& order) const;

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
