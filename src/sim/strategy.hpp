#pragma once
// The strategy interface (P3.10, architecture §11). A strategy reacts to callbacks and acts through its
// Context. Everything it sees arrives with latency: the book after feed latency, execution reports after
// ack latency; its requests reach the exchange after entry latency. The same interface is meant to run
// unchanged in the simulator and, later, against the live platform (live_platform_architecture.md, L5).
#include "observed_book.hpp"

#include "events.hpp"
#include "models.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace sim {

using OrderId = std::string;

// A participant's account in quote and base units (1e-8). Two copies exist per strategy: the exchange's
// (updated when a fill happens; the run's results) and the strategy's own (updated when it learns of the
// fill). The strategy reads only its own, so it can't know about a fill before the report reaches it.
struct Account {
    int64_t position = 0;      // base asset, signed
    int64_t cash = 0;          // quote asset: + sells, - buys (fees not included)
    int64_t fees = 0;          // quote, paid (negative: rebates)
    int64_t base_volume = 0;   // traded quantity
    int64_t quote_volume = 0;  // traded notional
    uint64_t fills = 0;
    uint64_t maker_fills = 0;
    uint64_t taker_fills = 0;

    // Value at a mark price (1e-8 quote units): cash + position x mark - fees, starting from zero
    int64_t pnl(Price mark) const;

    // Applies one FILL report
    void apply_fill(const ExecReport& fill);
};

enum class OrderStatus {
    PENDING_NEW,      // sent; no report yet
    NEW,              // acknowledged, resting
    PARTIALLY_FILLED,
    PENDING_CANCEL,   // cancel sent
    PENDING_REPLACE,  // replace sent
};

// An order as the strategy knows it (from what it sent and the reports it has received)
struct OpenOrder {
    OrderId id;
    Side side = Side::BUY;
    OrderType type = OrderType::LIMIT;
    Price price = 0;
    Quantity quantity = 0;   // total
    Quantity filled = 0;
    Quantity leaves = 0;     // open quantity as last reported (the full quantity while pending new)
    OrderStatus status = OrderStatus::PENDING_NEW;
    uint64_t sent_at = 0;    // sim time of the last request for this order
};

// A cancel, amend or replace the exchange refused. The order itself is unchanged (or already gone).
struct RequestReject {
    enum class Request { CANCEL, AMEND, REPLACE };
    Request request = Request::CANCEL;
    OrderId order_id;
    ExecReason reason = ExecReason::NONE;  // NONE: the order isn't open (unknown, filled or cancelled)
    std::string text;
    uint64_t exchange_time = 0;
};

const char* request_name(RequestReject::Request r);

class Context {
public:
    virtual ~Context() = default;

    // Sim time of the callback being run (ns since the epoch)
    virtual uint64_t now() const = 0;
    virtual const std::string& symbol() const = 0;
    virtual const std::string& owner() const = 0;

    // The market as the strategy sees it (delayed by feed latency)
    virtual const ObservedBook& book() const = 0;

    // False during the run's warm-up: the strategy sees the market but may not send, cancel, amend or
    // replace (those calls throw std::logic_error). Timers are allowed.
    virtual bool can_trade() const = 0;

    // Sends a new order; returns its ID. Validation happens at the exchange: a bad order comes back REJECTED.
    virtual OrderId send(Side side, OrderType type, Price price, Quantity quantity,
                         StpPolicy stp = StpPolicy::CANCEL_MAKER) = 0;
    virtual void cancel(const OrderId& id) = 0;
    // New total quantity (filled + open, as on Binance) and optionally a new price
    virtual void amend(const OrderId& id, Quantity new_total, std::optional<Price> new_price = std::nullopt) = 0;
    // Cancels the order and sends a new one under the same ID (loses queue priority). Refused, with no new
    // order, if the old one is no longer open when the request arrives.
    virtual void replace(const OrderId& id, Price price, Quantity quantity) = 0;

    // As known to the strategy
    virtual const Account& account() const = 0;
    virtual const std::map<OrderId, OpenOrder>& open_orders() const = 0;

    // A TIMER callback after delay_ns (no latency); returns its ID
    virtual uint64_t set_timer(uint64_t delay_ns) = 0;
    virtual void cancel_timer(uint64_t timer_id) = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual std::string name() const = 0;

    virtual void on_start(Context&) {}
    // A depth event or snapshot reached the strategy; book is ctx.book()
    virtual void on_book(Context&, const ObservedBook&) {}
    virtual void on_trade(Context&, const TradeEvent&) {}
    virtual void on_exec(Context&, const ExecReport&) {}
    virtual void on_reject(Context&, const RequestReject&) {}
    virtual void on_timer(Context&, uint64_t /*timer_id*/) {}
    // Updates were lost; the book is invalid until the next snapshot (on_book with ctx.book().valid())
    virtual void on_gap(Context&) {}
    virtual void on_end(Context&) {}
};

} // namespace sim
