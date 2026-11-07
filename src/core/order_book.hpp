#pragma once
#include "clock.hpp"
#include "events.hpp"
#include "fees.hpp"
#include "market_data.hpp"
#include "models.hpp"
#include "symbol_spec.hpp"
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// One price level: its orders in time priority, as an intrusive doubly linked list through
// Order::queue_prev / queue_next (oldest first). The level doesn't own them; OrderBook's lookup does.
struct PriceLevel {
    Price price = 0;
    Quantity total_quantity = 0;
    Order* head = nullptr;  // oldest order: matched first
    Order* tail = nullptr;  // newest order
    size_t count = 0;

    bool empty() const { return head == nullptr; }

    void push_back(Order& order) {
        order.queue_prev = tail;
        order.queue_next = nullptr;
        (tail ? tail->queue_next : head) = &order;
        tail = &order;
        order.queued = true;
        ++count;
    }

    // Unlinks a queued order of this level; its queue_next stays readable for an ongoing iteration
    void unlink(Order& order) {
        (order.queue_prev ? order.queue_prev->queue_next : head) = order.queue_next;
        (order.queue_next ? order.queue_next->queue_prev : tail) = order.queue_prev;
        order.queued = false;
        --count;
    }
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
    std::vector<Trade> submit_order(const std::shared_ptr<Order>& order, std::vector<ExecReport>& reports);

    // Cancels the resting order with the same ID (if any, reason REPLACED), then submits the new one.
    // The new order joins the back of its price level's queue.
    std::vector<Trade> replace_order(const std::shared_ptr<Order>& order, std::vector<ExecReport>& reports);

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

    // Starts one engine call (P1.18). Every report, trade and snapshot of the call carries `now`, so the
    // clock is read once per call rather than once per event. Reports are built only if `reporting`
    // (someone receives them); their sequence numbers advance either way, so a run's numbering doesn't
    // depend on who listens.
    void begin_call(uint64_t now, bool reporting) {
        now_ = now;
        reporting_ = reporting;
    }

    // Best Bid and Best Offer, timestamped with the current call's time
    Bbo bbo() const;

    // Top `levels` aggregated price levels per side, best first
    Depth depth(int levels = 10) const;

    // The resting orders at one price in queue order, or nullptr if there are none. Read-only (the
    // simulator's synthesizer inspects queues with it, P3.6); valid until the next mutating call.
    const PriceLevel* find_level(Side side, Price price) const;

    const std::string& get_symbol() const { return symbol_; }

    // Whether reports for market-data liquidity are built (EngineOptions::report_market_data_liquidity)
    void set_report_market_data(bool on) { report_market_data_ = on; }

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
    uint64_t now_ = 0;        // time of the current call (begin_call)
    bool reporting_ = true;   // build reports in the current call
    bool report_market_data_ = true;
    ExecReport discarded_;    // written instead of a report nobody receives
    uint64_t& trade_id_seq_;
    uint64_t& report_seq_;
    const FeeBook& fees_;
    std::optional<SymbolSpec> spec_;

    // Node memory for the price maps and the order lookup (P1.18): a pool per book, so resting, filling
    // and cancelling orders and creating and emptying levels don't go through the global heap. Declared
    // before the containers so that it outlives them.
    std::pmr::unsynchronized_pool_resource pool_;

    // Bids: Sorted in descending order (highest price first)
    std::pmr::map<Price, PriceLevel, std::greater<Price>> bids_{&pool_};

    // Asks: Sorted in ascending order (lowest price first)
    std::pmr::map<Price, PriceLevel, std::less<Price>> asks_{&pool_};

    // Resting orders by ID. The key views the order's own order_id (no copy of the ID); it stays valid
    // because the entry owns the order and a resting order's ID never changes.
    std::pmr::unordered_map<std::string_view, std::shared_ptr<Order>> orders_lookup_{&pool_};

    // Validates, reports first_report (ACK, or AMENDED for a modify), matches and rests or cancels the remainder
    std::vector<Trade> submit_internal(const std::shared_ptr<Order>& order, std::vector<ExecReport>& reports,
                                       ExecType first_report);

    // Rejection message for a post-only order that would trade
    std::string post_only_text(const Order& order) const;

    // Removes a resting order without reporting it. Returns the order, or nullptr if not found.
    std::shared_ptr<Order> erase_order(const std::string& order_id);

    // Appends a report describing the order's current state and returns it for further fields
    // (a scratch report when the call isn't reporting)
    ExecReport& report(std::vector<ExecReport>& reports, ExecType type, const Order& order,
                       ExecReason reason = ExecReason::NONE);

    // Matches the order against the opposite side of the book, best price first.
    // With enforce_limit, stops at the first level the order's price does not cross (LIMIT/IOC/FOK);
    // without it, sweeps until filled or the side is empty (MARKET). Never rests the remainder.
    // taker_cancelled is set when self-trade prevention cancelled the rest of the incoming order.
    std::vector<Trade> match_against_book(Order& order, bool enforce_limit, bool& taker_cancelled,
                                          std::vector<ExecReport>& reports);

    template <typename BookSide>
    void sweep(BookSide& book_side, Order& order, bool enforce_limit,
               std::vector<Trade>& trades, bool& taker_cancelled, std::vector<ExecReport>& reports);

    // True if a resting order at this price would cross the opposite side
    bool would_cross(const Order& order) const;

    // Dry-run simulation for FOK orders
    bool can_fok_be_fully_filled(const Order& order) const;

    template <typename BookSide>
    bool has_fillable_liquidity(const BookSide& book_side, const Order& order) const;

    template <typename BookSide>
    void check_side(const BookSide& book_side, Side side, std::vector<std::string>& violations,
                    size_t& resting_orders) const;

    // Helper to insert a resting order into the book
    void add_resting_order(const std::shared_ptr<Order>& order);

    // Helper to erase an order from the book data structures
    void remove_order_from_book(Order& order);

    // Unlinks a filled or STP-cancelled maker from its level and drops it from the lookup, which may
    // destroy it: always the last use of `maker`
    void release_maker(PriceLevel& level, Order& maker);
};
