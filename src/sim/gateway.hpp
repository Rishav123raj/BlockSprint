#pragma once
// SimGateway (P3.10, architecture §11): connects strategies to the exchange (the matching engine) through
// latency, the way a real connection would.
//
//   strategy side   Context calls (send / cancel / amend / replace / timers) become ORDER_ARRIVE events at
//                   entry latency; book batches, reports and request rejections are delivered after feed /
//                   ack latency, and only then change what the strategy knows (its book, open orders, account)
//   exchange side   arriving requests go through Binance's limits (orders per 10 s and per day, open orders),
//                   then into the engine. Every engine report for a strategy's order, whatever caused it (its
//                   own request, a data trade, another strategy), updates the exchange-side account and is
//                   sent back through the ack channel.
//
// The gateway doesn't run the event loop: it pushes events onto the simulator's queue and the loop (P3.11)
// hands them back through dispatch().
#include "event_queue.hpp"
#include "latency_models.hpp"
#include "observed_book.hpp"
#include "replayer.hpp"
#include "strategy.hpp"

#include "fees.hpp"
#include "matching_engine.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace sim {

// A strategy request on its way to the exchange
struct Request {
    enum class Type : uint8_t { NEW, CANCEL, AMEND, REPLACE };
    Type type = Type::NEW;
    OrderId order_id;
    Side side = Side::BUY;
    OrderType order_type = OrderType::LIMIT;
    Price price = 0;
    Quantity quantity = 0;                // NEW / REPLACE: quantity; AMEND: new total
    StpPolicy stp = StpPolicy::CANCEL_MAKER;
    std::optional<Price> new_price;       // AMEND
    uint64_t sent_at = 0;
};

struct TimerFire {
    uint64_t id = 0;
};

// What the simulator's event queue carries. The loop dispatches by class: MARKET_DATA (Batch, for the
// synthesizer), ORDER_ARRIVE (Request), STRATEGY_MD (Batch), STRATEGY_EXEC (ExecReport or RequestReject),
// TIMER (TimerFire).
struct SimEvent {
    uint32_t session = 0;
    std::variant<Batch, Request, ExecReport, RequestReject, TimerFire> body;
};
using SimQueue = EventQueue<SimEvent>;

// Binance spot defaults; 0 = no limit
struct GatewayLimits {
    uint32_t orders_per_10s = 100;
    uint32_t orders_per_day = 200000;
    uint32_t max_open_orders = 200;  // per symbol
};

struct SessionConfig {
    std::string owner;  // participant ID; order IDs are "<owner>-<n>"
    std::shared_ptr<const LatencyModel> feed;
    std::shared_ptr<const LatencyModel> entry;
    std::shared_ptr<const LatencyModel> ack;
    FeeSchedule fees{};
    GatewayLimits limits{};
};

// Sees every message between the strategies and the exchange, e.g. the result recorder (P3.13). Called
// when the message is sent, with the time it will be delivered.
class GatewayListener {
public:
    virtual ~GatewayListener() = default;
    // A request leaving a strategy (now = sent_at); it reaches the exchange at arrive_at
    virtual void on_request(uint32_t /*session*/, const Request& /*r*/, uint64_t /*arrive_at*/) {}
    // A report for a strategy's order at exchange time; the strategy receives it at deliver_at
    virtual void on_report(uint32_t /*session*/, const ExecReport& /*r*/, uint64_t /*deliver_at*/) {}
    // A refused cancel / amend / replace at exchange time; the strategy learns of it at deliver_at
    virtual void on_request_reject(uint32_t /*session*/, const RequestReject& /*r*/, uint64_t /*deliver_at*/) {}
};

struct SessionStats {
    uint64_t orders_sent = 0;     // new orders
    uint64_t cancels_sent = 0;
    uint64_t amends_sent = 0;
    uint64_t replaces_sent = 0;
    uint64_t rate_limited = 0;    // rejected by orders per 10 s / per day
    uint64_t open_limited = 0;    // rejected by max open orders
    uint64_t request_rejects = 0; // cancels / amends / replaces refused
    uint64_t reports = 0;         // execution reports delivered
    uint64_t md_batches = 0;      // book and trade batches delivered
    uint64_t timers = 0;          // timers fired
    uint64_t held_back = 0;       // messages delayed to keep a channel in order (all three channels)
};

class SimGateway {
public:
    // Installs itself as the engine's execution report callback. The engine, queue and strategies must
    // outlive the gateway.
    SimGateway(MatchingEngine& engine, SimQueue& queue, std::string symbol, int64_t tick_size, int64_t lot_size,
               uint64_t seed);
    ~SimGateway();
    SimGateway(const SimGateway&) = delete;
    SimGateway& operator=(const SimGateway&) = delete;

    // Adds a strategy connection; returns its session index. Latency streams are named after the owner, so
    // a session's delays don't depend on the other sessions.
    uint32_t add_session(Strategy& strategy, SessionConfig config);
    size_t session_count() const { return sessions_.size(); }

    // Loop interface
    void start();                              // on_start of every strategy, at the clock's time
    void publish(const Batch& batch);          // a batch was applied at the exchange now: deliver it after feed latency
    void observe(const Batch& batch);          // fast-forward: update the observed books now, no latency or callbacks
    void set_trading_from(uint64_t t) { trading_from_ = t; }  // requests before t throw (warm-up)
    void dispatch(const SimQueue::Event& e);   // ORDER_ARRIVE, STRATEGY_MD, STRATEGY_EXEC and TIMER events
    void end();                                // on_end of every strategy

    // Results
    const Account& exchange_account(uint32_t session) const;   // fills as they happened
    const Account& strategy_account(uint32_t session) const;   // fills the strategy has learned of
    SessionStats stats(uint32_t session) const;
    Context& context(uint32_t session);
    const std::string& owner(uint32_t session) const;
    const Strategy& strategy(uint32_t session) const;
    size_t exchange_open_orders(uint32_t session) const;  // as the exchange knows them

    // nullptr to remove; the listener must outlive the gateway or be removed first
    void set_listener(GatewayListener* listener) { listener_ = listener; }
    // Called with the trades each strategy request made as the taker (new, replace, amend), at exchange
    // time, e.g. for the impact overlay (P6.1)
    void set_taker_trades(std::function<void(const std::vector<Trade>&)> fn) { taker_trades_ = std::move(fn); }

private:
    class Session;

    void on_engine_report(const ExecReport& r);
    uint64_t now() const { return engine_.clock().now_ns(); }

    MatchingEngine& engine_;
    SimQueue& queue_;
    std::string symbol_;
    int64_t tick_;
    int64_t lot_;
    uint64_t seed_;
    uint64_t trading_from_ = 0;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::unordered_map<std::string, uint32_t> by_owner_;
    GatewayListener* listener_ = nullptr;
    std::function<void(const std::vector<Trade>&)> taker_trades_;
};

} // namespace sim
