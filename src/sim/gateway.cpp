#include "gateway.hpp"

#include <stdexcept>

namespace sim {

const char* request_name(RequestReject::Request r) {
    switch (r) {
        case RequestReject::Request::CANCEL: return "cancel";
        case RequestReject::Request::AMEND: return "amend";
        case RequestReject::Request::REPLACE: return "replace";
    }
    return "unknown";
}

int64_t Account::pnl(Price mark) const {
    const int64_t value = position >= 0 ? notional_for(mark, position) : -notional_for(mark, -position);
    return cash + value - fees;
}

void Account::apply_fill(const ExecReport& fill) {
    const int64_t notional = notional_for(fill.last_px, fill.last_qty);
    if (fill.side == Side::BUY) {
        position += fill.last_qty;
        cash -= notional;
    } else {
        position -= fill.last_qty;
        cash += notional;
    }
    fees += fill.fee;
    base_volume += fill.last_qty;
    quote_volume += notional;
    ++fills;
    if (fill.liquidity == Liquidity::MAKER) {
        ++maker_fills;
    } else if (fill.liquidity == Liquidity::TAKER) {
        ++taker_fills;
    }
}

namespace {

constexpr uint64_t kTenSeconds = 10ULL * 1000000000ULL;
constexpr uint64_t kDay = 86400ULL * 1000000000ULL;
constexpr uint64_t kMinute = 60ULL * 1000000000ULL;

} // namespace

// One strategy's connection: the Context it acts through, its latency channels, what it knows, and the
// exchange's view of it
class SimGateway::Session final : public Context {
public:
    Session(SimGateway& gw, uint32_t index, Strategy& strategy, SessionConfig cfg)
        : gw_(gw), index_(index), strategy_(strategy), cfg_(std::move(cfg)), book_(gw.tick_, gw.lot_),
          feed_(*cfg_.feed, stream_rng(gw.seed_, "feed/" + cfg_.owner)),
          entry_(*cfg_.entry, stream_rng(gw.seed_, "entry/" + cfg_.owner)),
          ack_(*cfg_.ack, stream_rng(gw.seed_, "ack/" + cfg_.owner)) {}

    // ---- Context (strategy side) ----
    uint64_t now() const override { return gw_.now(); }
    const std::string& symbol() const override { return gw_.symbol_; }
    const std::string& owner() const override { return cfg_.owner; }
    const ObservedBook& book() const override { return book_; }
    const Account& account() const override { return known_account_; }
    const std::map<OrderId, OpenOrder>& open_orders() const override { return known_orders_; }
    bool can_trade() const override { return now() >= gw_.trading_from_; }

    OrderId send(Side side, OrderType type, Price price, Quantity quantity, StpPolicy stp) override {
        require_trading("send");
        Request r;
        r.type = Request::Type::NEW;
        r.order_id = cfg_.owner + "-" + std::to_string(++order_seq_);
        r.side = side;
        r.order_type = type;
        r.price = price;
        r.quantity = quantity;
        r.stp = stp;
        OpenOrder& o = known_orders_[r.order_id];
        o.id = r.order_id;
        o.side = side;
        o.type = type;
        o.price = price;
        o.quantity = quantity;
        o.leaves = quantity;
        o.status = OrderStatus::PENDING_NEW;
        o.sent_at = now();
        ++stats_.orders_sent;
        OrderId id = r.order_id;
        to_exchange(std::move(r));
        return id;
    }

    // Side and type of the order a cancel / amend / replace is for (for the exchange and the listener)
    void describe(Request& r) const {
        if (auto it = known_orders_.find(r.order_id); it != known_orders_.end()) {
            r.side = it->second.side;
            r.order_type = it->second.type;
        }
    }

    void cancel(const OrderId& id) override {
        require_trading("cancel");
        Request r;
        r.type = Request::Type::CANCEL;
        r.order_id = id;
        describe(r);
        mark_pending(id, OrderStatus::PENDING_CANCEL);
        ++stats_.cancels_sent;
        to_exchange(std::move(r));
    }

    void amend(const OrderId& id, Quantity new_total, std::optional<Price> new_price) override {
        require_trading("amend");
        Request r;
        r.type = Request::Type::AMEND;
        r.order_id = id;
        describe(r);
        r.quantity = new_total;
        r.new_price = new_price;
        ++stats_.amends_sent;
        to_exchange(std::move(r));
    }

    void replace(const OrderId& id, Price price, Quantity quantity) override {
        require_trading("replace");
        Request r;
        r.type = Request::Type::REPLACE;
        r.order_id = id;
        r.price = price;
        r.quantity = quantity;
        describe(r);
        r.stp = StpPolicy::CANCEL_MAKER;
        mark_pending(id, OrderStatus::PENDING_REPLACE);
        ++stats_.replaces_sent;
        to_exchange(std::move(r));
    }

    uint64_t set_timer(uint64_t delay_ns) override {
        const uint64_t id = ++timer_seq_;
        gw_.queue_.push(now() + delay_ns, EventClass::TIMER, SimEvent{index_, TimerFire{id}});
        return id;
    }

    void cancel_timer(uint64_t timer_id) override { cancelled_timers_.insert(timer_id); }

    // ---- deliveries (strategy side, at the event's time) ----
    void deliver_md(const Batch& batch) {
        ++stats_.md_batches;
        book_.apply(batch, now());
        switch (batch.kind) {
            case BatchKind::TRADE:
                strategy_.on_trade(*this, *book_.last_trade());
                break;
            case BatchKind::GAP:
                strategy_.on_gap(*this);
                break;
            case BatchKind::DEPTH:
            case BatchKind::SNAPSHOT:
                strategy_.on_book(*this, book_);
                break;
        }
    }

    void deliver_exec(const ExecReport& r) {
        ++stats_.reports;
        auto it = known_orders_.find(r.order_id);
        switch (r.type) {
            case ExecType::REJECTED:
                if (it != known_orders_.end()) {
                    known_orders_.erase(it);
                }
                break;
            case ExecType::ACK:
            case ExecType::AMENDED: {
                OpenOrder& o = known_orders_[r.order_id];
                o.id = r.order_id;
                o.side = r.side;
                o.type = r.order_type;
                o.price = r.price;
                o.quantity = r.quantity;
                o.filled = r.cum_qty;
                o.leaves = r.leaves_qty;
                o.status = r.cum_qty > 0 ? OrderStatus::PARTIALLY_FILLED : OrderStatus::NEW;
                break;
            }
            case ExecType::FILL:
                known_account_.apply_fill(r);
                if (it != known_orders_.end()) {
                    if (r.leaves_qty == 0) {
                        known_orders_.erase(it);
                    } else {
                        it->second.filled = r.cum_qty;
                        it->second.leaves = r.leaves_qty;
                        if (it->second.status == OrderStatus::NEW || it->second.status == OrderStatus::PENDING_NEW) {
                            it->second.status = OrderStatus::PARTIALLY_FILLED;
                        }
                    }
                }
                break;
            case ExecType::CANCELLED:
                // A replace cancels the old order first; the entry stays for the new one's ACK
                if (it != known_orders_.end() && r.reason != ExecReason::REPLACED) {
                    known_orders_.erase(it);
                }
                break;
        }
        strategy_.on_exec(*this, r);
    }

    void deliver_reject(const RequestReject& rr) {
        ++stats_.request_rejects;
        // The order is unchanged: undo the pending status
        if (auto it = known_orders_.find(rr.order_id); it != known_orders_.end()) {
            OpenOrder& o = it->second;
            if (o.status == OrderStatus::PENDING_CANCEL || o.status == OrderStatus::PENDING_REPLACE) {
                o.status = o.filled > 0 ? OrderStatus::PARTIALLY_FILLED : OrderStatus::NEW;
            }
        }
        strategy_.on_reject(*this, rr);
    }

    void fire_timer(uint64_t id) {
        if (cancelled_timers_.erase(id)) {
            return;
        }
        ++stats_.timers;
        strategy_.on_timer(*this, id);
    }

    void publish(const Batch& batch) {
        gw_.queue_.push(feed_.arrival(now()), EventClass::STRATEGY_MD, SimEvent{index_, batch});
    }

    void observe(const Batch& batch) { book_.apply(batch, now()); }

    // ---- exchange side ----
    void arrive(const Request& r) {
        const uint64_t t = now();
        if (!check_weight(r, t)) {
            ++stats_.weight_limited;
            const std::string text = "Too much request weight used; current limit is " +
                                     std::to_string(cfg_.limits.request_weight_per_min) + " request weight per 1 MINUTE";
            switch (r.type) {
                case Request::Type::NEW: reject_order(r, ExecReason::RATE_LIMIT, text); break;
                case Request::Type::CANCEL:
                    reject_request(RequestReject::Request::CANCEL, r.order_id, ExecReason::RATE_LIMIT, text);
                    break;
                case Request::Type::AMEND:
                    reject_request(RequestReject::Request::AMEND, r.order_id, ExecReason::RATE_LIMIT, text);
                    break;
                case Request::Type::REPLACE:
                    reject_request(RequestReject::Request::REPLACE, r.order_id, ExecReason::RATE_LIMIT, text);
                    break;
            }
            return;
        }
        switch (r.type) {
            case Request::Type::NEW:
            case Request::Type::REPLACE: {
                if (auto reason = check_rate(t)) {
                    ++stats_.rate_limited;
                    if (r.type == Request::Type::NEW) {
                        reject_order(r, *reason, "Too many orders: " + rate_text());
                    } else {
                        reject_request(RequestReject::Request::REPLACE, r.order_id, *reason, "Too many orders: " +
                                                                                                  rate_text());
                    }
                    return;
                }
                if (r.type == Request::Type::REPLACE && !exchange_open_.count(r.order_id)) {
                    reject_request(RequestReject::Request::REPLACE, r.order_id, ExecReason::NONE, "Order is not open");
                    return;
                }
                if (r.type == Request::Type::NEW && cfg_.limits.max_open_orders &&
                    exchange_open_.size() >= cfg_.limits.max_open_orders) {
                    ++stats_.open_limited;
                    reject_order(r, ExecReason::MAX_OPEN_ORDERS,
                                 "Maximum of " + std::to_string(cfg_.limits.max_open_orders) + " open orders reached");
                    return;
                }
                auto order = std::make_shared<Order>();
                order->order_id = r.order_id;
                order->symbol = gw_.symbol_;
                order->order_type = r.order_type;
                order->side = r.side;
                order->price = r.price;
                order->quantity = r.quantity;
                order->timestamp = t;
                order->owner_id = cfg_.owner;
                order->stp_policy = r.stp;
                const auto trades = r.type == Request::Type::NEW ? gw_.engine_.submit_order(order)
                                                                 : gw_.engine_.replace_order(order);
                if (gw_.taker_trades_ && !trades.empty()) {
                    gw_.taker_trades_(trades);
                }
                return;
            }
            case Request::Type::CANCEL:
                if (!gw_.engine_.cancel_order(gw_.symbol_, r.order_id)) {
                    reject_request(RequestReject::Request::CANCEL, r.order_id, ExecReason::NONE, "Order is not open");
                }
                return;
            case Request::Type::AMEND: {
                if (!exchange_open_.count(r.order_id)) {
                    reject_request(RequestReject::Request::AMEND, r.order_id, ExecReason::NONE, "Order is not open");
                    return;
                }
                std::vector<Trade> trades;
                amending_ = &r.order_id;
                gw_.engine_.amend_order(gw_.symbol_, r.order_id, r.quantity, r.new_price, trades);
                amending_ = nullptr;
                if (gw_.taker_trades_ && !trades.empty()) {
                    gw_.taker_trades_(trades);
                }
                return;
            }
        }
    }

    // A report from the engine for one of this session's orders, at exchange time
    void exchange_report(const ExecReport& r) {
        if (amending_ && r.type == ExecType::REJECTED && r.order_id == *amending_) {
            // The engine refused an amend: the order is unchanged. Tell the strategy as a request reject.
            reject_request(RequestReject::Request::AMEND, r.order_id, r.reason, r.text);
            return;
        }
        if (is_terminal(r)) {
            exchange_open_.erase(r.order_id);
        } else {
            exchange_open_.insert(r.order_id);
        }
        if (r.type == ExecType::FILL) {
            exchange_account_.apply_fill(r);
        }
        to_strategy(r);
    }

    Strategy& strategy() { return strategy_; }
    const Account& exchange_account() const { return exchange_account_; }
    size_t exchange_open_orders() const { return exchange_open_.size(); }
    SessionStats stats() const {
        SessionStats s = stats_;
        s.held_back = feed_.held_back() + entry_.held_back() + ack_.held_back();
        return s;
    }

private:
    void require_trading(const char* what) const {
        if (!can_trade()) {
            throw std::logic_error("strategy '" + cfg_.owner + "': " + what + " during the warm-up (trading starts at " +
                                   std::to_string(gw_.trading_from_) + " ns); check ctx.can_trade()");
        }
    }

    void mark_pending(const OrderId& id, OrderStatus status) {
        if (auto it = known_orders_.find(id); it != known_orders_.end()) {
            it->second.status = status;
            it->second.sent_at = now();
        }
    }

    void to_exchange(Request r) {
        r.sent_at = now();
        const uint64_t arrive = entry_.arrival(r.sent_at);
        if (gw_.listener_) {
            gw_.listener_->on_request(index_, r, arrive);
        }
        gw_.queue_.push(arrive, EventClass::ORDER_ARRIVE, SimEvent{index_, std::move(r)});
    }

    void to_strategy(const ExecReport& r) {
        const uint64_t deliver = ack_.arrival(now());
        if (gw_.listener_) {
            gw_.listener_->on_report(index_, r, deliver);
        }
        gw_.queue_.push(deliver, EventClass::STRATEGY_EXEC, SimEvent{index_, r});
    }

    void reject_request(RequestReject::Request request, const OrderId& id, ExecReason reason, std::string text) {
        RequestReject rr;
        rr.request = request;
        rr.order_id = id;
        rr.reason = reason;
        rr.text = std::move(text);
        rr.exchange_time = now();
        const uint64_t deliver = ack_.arrival(now());
        if (gw_.listener_) {
            gw_.listener_->on_request_reject(index_, rr, deliver);
        }
        gw_.queue_.push(deliver, EventClass::STRATEGY_EXEC, SimEvent{index_, std::move(rr)});
    }

    // A new order refused before the engine: one REJECTED report, as the engine would send (seq 0: it
    // isn't one of the engine's reports)
    void reject_order(const Request& r, ExecReason reason, std::string text) {
        ExecReport rep;
        rep.timestamp = now();
        rep.type = ExecType::REJECTED;
        rep.reason = reason;
        rep.text = std::move(text);
        rep.order_id = r.order_id;
        rep.owner_id = cfg_.owner;
        rep.symbol = gw_.symbol_;
        rep.side = r.side;
        rep.order_type = r.order_type;
        rep.price = r.price;
        rep.quantity = r.quantity;
        to_strategy(rep);
    }

    // Charges a request's weight to the current minute; false if it doesn't fit (and then charges nothing)
    bool check_weight(const Request& r, uint64_t t) {
        if (!cfg_.limits.request_weight_per_min) return true;
        if (t / kMinute != minute_) {
            minute_ = t / kMinute;
            weight_used_ = 0;
        }
        const GatewayLimits& l = cfg_.limits;
        uint32_t w = 0;
        switch (r.type) {
            case Request::Type::NEW: w = l.weight_new; break;
            case Request::Type::CANCEL: w = l.weight_cancel; break;
            case Request::Type::AMEND: w = l.weight_amend; break;
            case Request::Type::REPLACE: w = l.weight_replace; break;
        }
        if (weight_used_ + w > l.request_weight_per_min) return false;
        weight_used_ += w;
        return true;
    }

    // Counts an order against the limits; returns the reason if it breaks one (and then doesn't count it)
    std::optional<ExecReason> check_rate(uint64_t t) {
        while (!recent_.empty() && recent_.front() + kTenSeconds <= t) {
            recent_.pop_front();
        }
        if (t / kDay != day_) {
            day_ = t / kDay;
            today_ = 0;
        }
        if ((cfg_.limits.orders_per_10s && recent_.size() >= cfg_.limits.orders_per_10s) ||
            (cfg_.limits.orders_per_day && today_ >= cfg_.limits.orders_per_day)) {
            return ExecReason::RATE_LIMIT;
        }
        recent_.push_back(t);
        ++today_;
        return std::nullopt;
    }

    std::string rate_text() const {
        return std::to_string(cfg_.limits.orders_per_10s) + " per 10 s, " + std::to_string(cfg_.limits.orders_per_day) +
               " per day";
    }

    SimGateway& gw_;
    uint32_t index_;
    Strategy& strategy_;
    SessionConfig cfg_;
    ObservedBook book_;
    LatencyChannel feed_;
    LatencyChannel entry_;
    LatencyChannel ack_;

    // What the strategy knows
    Account known_account_;
    std::map<OrderId, OpenOrder> known_orders_;
    uint64_t order_seq_ = 0;
    uint64_t timer_seq_ = 0;
    std::unordered_set<uint64_t> cancelled_timers_;

    // What the exchange knows
    Account exchange_account_;
    std::unordered_set<OrderId> exchange_open_;
    std::deque<uint64_t> recent_;  // arrival times of orders in the last 10 s
    uint64_t day_ = 0;
    uint64_t today_ = 0;
    uint64_t minute_ = 0;
    uint64_t weight_used_ = 0;  // request weight charged in the current minute
    const OrderId* amending_ = nullptr;  // the order of an amend being processed

    SessionStats stats_;
};

SimGateway::SimGateway(MatchingEngine& engine, SimQueue& queue, std::string symbol, int64_t tick_size,
                       int64_t lot_size, uint64_t seed)
    : engine_(engine), queue_(queue), symbol_(std::move(symbol)), tick_(tick_size), lot_(lot_size), seed_(seed) {
    engine_.register_exec_report_callback([this](const ExecReport& r) { on_engine_report(r); });
}

SimGateway::~SimGateway() { engine_.register_exec_report_callback(nullptr); }

uint32_t SimGateway::add_session(Strategy& strategy, SessionConfig config) {
    if (config.owner.empty() || config.owner == "MARKET" || config.owner == "MARKET_TAPE") {
        throw std::invalid_argument("a strategy needs an owner ID other than MARKET / MARKET_TAPE");
    }
    if (by_owner_.count(config.owner)) {
        throw std::invalid_argument("duplicate strategy owner '" + config.owner + "'");
    }
    if (!config.feed || !config.entry || !config.ack) {
        throw std::invalid_argument("strategy '" + config.owner + "' needs feed, entry and ack latency models");
    }
    engine_.set_fee_schedule(config.owner, config.fees);
    const auto index = static_cast<uint32_t>(sessions_.size());
    by_owner_[config.owner] = index;
    sessions_.push_back(std::make_unique<Session>(*this, index, strategy, std::move(config)));
    return index;
}

void SimGateway::start() {
    for (auto& s : sessions_) {
        s->strategy().on_start(*s);
    }
}

void SimGateway::end() {
    for (auto& s : sessions_) {
        s->strategy().on_end(*s);
    }
}

void SimGateway::publish(const Batch& batch) {
    for (auto& s : sessions_) {
        s->publish(batch);
    }
}

void SimGateway::observe(const Batch& batch) {
    for (auto& s : sessions_) {
        s->observe(batch);
    }
}

void SimGateway::dispatch(const SimQueue::Event& e) {
    Session& s = *sessions_.at(e.payload.session);
    const auto& body = e.payload.body;
    switch (e.cls) {
        case EventClass::ORDER_ARRIVE:
            s.arrive(std::get<Request>(body));
            return;
        case EventClass::STRATEGY_MD:
            s.deliver_md(std::get<Batch>(body));
            return;
        case EventClass::STRATEGY_EXEC:
            if (const auto* r = std::get_if<ExecReport>(&body)) {
                s.deliver_exec(*r);
            } else {
                s.deliver_reject(std::get<RequestReject>(body));
            }
            return;
        case EventClass::TIMER:
            s.fire_timer(std::get<TimerFire>(body).id);
            return;
        case EventClass::MARKET_DATA:
            break;
    }
    throw std::logic_error("SimGateway::dispatch: market data events belong to the loop");
}

void SimGateway::on_engine_report(const ExecReport& r) {
    if (r.owner_id.empty()) {
        return;
    }
    auto it = by_owner_.find(r.owner_id);
    if (it != by_owner_.end()) {
        sessions_[it->second]->exchange_report(r);
    }
}

const Account& SimGateway::exchange_account(uint32_t session) const { return sessions_.at(session)->exchange_account(); }

const Account& SimGateway::strategy_account(uint32_t session) const { return sessions_.at(session)->account(); }

SessionStats SimGateway::stats(uint32_t session) const { return sessions_.at(session)->stats(); }

Context& SimGateway::context(uint32_t session) { return *sessions_.at(session); }

const std::string& SimGateway::owner(uint32_t session) const { return sessions_.at(session)->owner(); }

const Strategy& SimGateway::strategy(uint32_t session) const { return sessions_.at(session)->strategy(); }

size_t SimGateway::exchange_open_orders(uint32_t session) const { return sessions_.at(session)->exchange_open_orders(); }

} // namespace sim
