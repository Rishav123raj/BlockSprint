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

const char* gap_policy_name(GapPolicy p) {
    switch (p) {
        case GapPolicy::HALT: return "halt";
        case GapPolicy::FLATTEN: return "flatten";
        case GapPolicy::CONTINUE: return "continue";
    }
    return "unknown";
}

GapPolicy parse_gap_policy(const std::string& name) {
    for (GapPolicy p : {GapPolicy::HALT, GapPolicy::FLATTEN, GapPolicy::CONTINUE}) {
        if (name == gap_policy_name(p)) return p;
    }
    throw std::invalid_argument("unknown gap policy '" + name + "' (halt, flatten or continue)");
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
        : gw_(gw), index_(index), strategy_(strategy), cfg_(std::move(cfg)),
          feed_(*cfg_.feed, stream_rng(gw.seed_, "feed/" + cfg_.owner)),
          entry_(*cfg_.entry, stream_rng(gw.seed_, "entry/" + cfg_.owner)),
          ack_(*cfg_.ack, stream_rng(gw.seed_, "ack/" + cfg_.owner)),
          risk_(cfg_.risk) {
        legs_.reserve(gw.symbols_.size());
        for (uint32_t i = 0; i < gw.symbols_.size(); ++i) {
            const SymbolInfo& s = gw.symbols_[i];
            legs_.emplace_back(ObservedBook(s.tick_size, s.lot_size, s.name, i));
        }
    }

    // ---- Context (strategy side) ----
    uint64_t now() const override { return gw_.now(); }
    const std::string& symbol() const override { return gw_.names_[0]; }
    const std::vector<std::string>& symbols() const override { return gw_.names_; }
    const std::string& owner() const override { return cfg_.owner; }
    const ObservedBook& book() const override { return legs_[0].book; }
    const ObservedBook& book(const std::string& symbol) const override {
        return legs_[gw_.symbol_index(symbol)].book;
    }
    const Account& account() const override { return legs_[0].known; }
    const Account& account(const std::string& symbol) const override {
        return legs_[gw_.symbol_index(symbol)].known;
    }
    const std::map<OrderId, OpenOrder>& open_orders() const override { return known_orders_; }
    bool can_trade() const override { return now() >= gw_.trading_from_; }

    OrderId send(Side side, OrderType type, Price price, Quantity quantity, StpPolicy stp) override {
        return send_on(0, side, type, price, quantity, stp);
    }

    OrderId send(const std::string& symbol, Side side, OrderType type, Price price, Quantity quantity,
                 StpPolicy stp) override {
        return send_on(gw_.symbol_index(symbol), side, type, price, quantity, stp);
    }

    OrderId send_on(uint32_t sym, Side side, OrderType type, Price price, Quantity quantity, StpPolicy stp) {
        require_trading("send");
        Leg& leg = legs_[sym];
        Request r;
        r.type = Request::Type::NEW;
        r.order_id = cfg_.owner + "-" + std::to_string(++order_seq_);
        r.symbol = sym;
        r.side = side;
        r.order_type = type;
        r.price = price;
        r.quantity = quantity;
        r.stp = stp;
        // Checked against the strategy's other orders, before this one joins them
        RiskVerdict verdict;
        if (leg.halted) {
            verdict = halt_verdict();
        } else if (risk_.limits().any() && !flattening_) {
            verdict = risk_.check_order(side, type, price, quantity, leg.known, known_orders_, leg.book);
        }
        OpenOrder& o = known_orders_[r.order_id];
        o.id = r.order_id;
        o.symbol = gw_.names_[sym];
        o.side = side;
        o.type = type;
        o.price = price;
        o.quantity = quantity;
        o.leaves = quantity;
        o.status = OrderStatus::PENDING_NEW;
        o.sent_at = now();
        ++stats_.orders_sent;
        OrderId id = r.order_id;
        if (!verdict.ok()) {
            ++(leg.halted ? stats_.gap_rejects : stats_.risk_rejects);
            r.sent_at = now();
            if (gw_.listener_) gw_.listener_->on_request(index_, r, r.sent_at);
            reject_order(r, verdict.reason, verdict.text, true);
            return id;
        }
        to_exchange(std::move(r));
        return id;
    }

    // Symbol, side and type of the order a cancel / amend / replace is for (for the exchange and the
    // listener); an order the strategy doesn't know is taken to be on the primary symbol
    void describe(Request& r) const {
        if (auto it = known_orders_.find(r.order_id); it != known_orders_.end()) {
            r.symbol = gw_.symbol_index(it->second.symbol);
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
        const Leg& leg = legs_[r.symbol];
        if (leg.halted || risk_.limits().any()) {
            if (auto it = known_orders_.find(id); it != known_orders_.end()) {
                const OpenOrder& o = it->second;
                const Quantity leaves = new_total - o.filled;
                if (leaves > o.leaves || (new_price && *new_price != o.price)) {
                    if (leg.halted) {
                        refuse(r, RequestReject::Request::AMEND, halt_verdict());
                        return;
                    }
                    const RiskVerdict v = risk_.check_order(o.side, o.type, new_price.value_or(o.price), leaves,
                                                            leg.known, known_orders_, leg.book, &id);
                    if (!v.ok()) {
                        refuse(r, RequestReject::Request::AMEND, v);
                        return;
                    }
                }
            }
        }
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
        ++stats_.replaces_sent;
        const Leg& leg = legs_[r.symbol];
        if (leg.halted && known_orders_.count(id)) {
            refuse(r, RequestReject::Request::REPLACE, halt_verdict());
            return;
        }
        if (risk_.limits().any() && known_orders_.count(id)) {
            const RiskVerdict v = risk_.check_order(r.side, r.order_type, price, quantity, leg.known, known_orders_,
                                                    leg.book, &id);
            if (!v.ok()) {
                refuse(r, RequestReject::Request::REPLACE, v);
                return;
            }
        }
        mark_pending(id, OrderStatus::PENDING_REPLACE);
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
        Leg& leg = legs_[batch.symbol];
        leg.book.apply(batch, now());
        if (batch.kind == BatchKind::GAP) {
            on_feed_gap(batch.symbol);
        } else if (leg.halted && leg.book.valid()) {
            resume(batch.symbol);
        }
        check_drawdown();
        switch (batch.kind) {
            case BatchKind::TRADE:
                strategy_.on_trade(*this, *leg.book.last_trade());
                break;
            case BatchKind::GAP:
                strategy_.on_gap(*this);
                break;
            case BatchKind::DEPTH:
            case BatchKind::SNAPSHOT:
                strategy_.on_book(*this, leg.book);
                break;
        }
    }

    void deliver_exec(const ExecReport& r) {
        ++stats_.reports;
        bool fill_seen = false;
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
                o.symbol = r.symbol;
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
                legs_[gw_.symbol_index(r.symbol)].known.apply_fill(r);
                fill_seen = true;
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
        if (fill_seen) check_drawdown();
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
        gw_.queue_.push(feed_.arrival(now(), gw_.latency_scale(Channel::FEED)), EventClass::STRATEGY_MD,
                        SimEvent{index_, batch});
    }

    void observe(const Batch& batch) { legs_[batch.symbol].book.apply(batch, now()); }

    // ---- exchange side ----
    void arrive(const Request& r) {
        const uint64_t t = now();
        Leg& leg = legs_[r.symbol];
        const std::string& symbol = gw_.names_[r.symbol];
        if (!check_weight(r, t)) {
            ++stats_.weight_limited;
            const std::string text = "Too much request weight used; current limit is " +
                                     std::to_string(cfg_.limits.request_weight_per_min) + " request weight per 1 MINUTE";
            switch (r.type) {
                case Request::Type::NEW: reject_order(r, ExecReason::RATE_LIMIT, text); break;
                case Request::Type::CANCEL:
                    reject_request(RequestReject::Request::CANCEL, r, ExecReason::RATE_LIMIT, text);
                    break;
                case Request::Type::AMEND:
                    reject_request(RequestReject::Request::AMEND, r, ExecReason::RATE_LIMIT, text);
                    break;
                case Request::Type::REPLACE:
                    reject_request(RequestReject::Request::REPLACE, r, ExecReason::RATE_LIMIT, text);
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
                        reject_request(RequestReject::Request::REPLACE, r, *reason, "Too many orders: " + rate_text());
                    }
                    return;
                }
                if (r.type == Request::Type::REPLACE && !leg.exchange_open.count(r.order_id)) {
                    reject_request(RequestReject::Request::REPLACE, r, ExecReason::NONE, "Order is not open");
                    return;
                }
                if (r.type == Request::Type::NEW && cfg_.limits.max_open_orders &&
                    leg.exchange_open.size() >= cfg_.limits.max_open_orders) {
                    ++stats_.open_limited;
                    reject_order(r, ExecReason::MAX_OPEN_ORDERS,
                                 "Maximum of " + std::to_string(cfg_.limits.max_open_orders) + " open orders reached");
                    return;
                }
                auto order = std::make_shared<Order>();
                order->order_id = r.order_id;
                order->symbol = symbol;
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
                if (!gw_.engine_.cancel_order(symbol, r.order_id)) {
                    reject_request(RequestReject::Request::CANCEL, r, ExecReason::NONE, "Order is not open");
                }
                return;
            case Request::Type::AMEND: {
                if (!leg.exchange_open.count(r.order_id)) {
                    reject_request(RequestReject::Request::AMEND, r, ExecReason::NONE, "Order is not open");
                    return;
                }
                std::vector<Trade> trades;
                amending_ = &r;
                gw_.engine_.amend_order(symbol, r.order_id, r.quantity, r.new_price, trades);
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
        if (amending_ && r.type == ExecType::REJECTED && r.order_id == amending_->order_id) {
            // The engine refused an amend: the order is unchanged. Tell the strategy as a request reject.
            reject_request(RequestReject::Request::AMEND, *amending_, r.reason, r.text);
            return;
        }
        Leg& leg = legs_[gw_.symbol_index(r.symbol)];
        if (is_terminal(r)) {
            leg.exchange_open.erase(r.order_id);
        } else {
            leg.exchange_open.insert(r.order_id);
        }
        if (r.type == ExecType::FILL) {
            leg.exchange.apply_fill(r);
        }
        to_strategy(r);
    }

    Strategy& strategy() { return strategy_; }
    const Account& exchange_account(uint32_t sym) const { return legs_.at(sym).exchange; }
    const Account& strategy_account(uint32_t sym) const { return legs_.at(sym).known; }
    size_t exchange_open_orders() const {
        size_t n = 0;
        for (const Leg& l : legs_) n += l.exchange_open.size();
        return n;
    }
    size_t exchange_open_orders(uint32_t sym) const { return legs_.at(sym).exchange_open.size(); }
    SessionStats stats() const {
        SessionStats s = stats_;
        for (const Leg& l : legs_) {
            if (l.halted) s.halted_ns += now() - l.halted_since;
        }
        s.held_back = feed_.held_back() + entry_.held_back() + ack_.held_back();
        return s;
    }

private:
    // What the session keeps per symbol
    struct Leg {
        explicit Leg(ObservedBook b) : book(std::move(b)) {}
        ObservedBook book;
        Account known;     // as the strategy knows it
        Account exchange;  // as the exchange knows it
        std::unordered_set<OrderId> exchange_open;
        bool halted = false;  // gap policy: waiting for a snapshot
        uint64_t halted_since = 0;
    };

    // A request the RiskGuard refused: recorded as sent, refused at once (no latency)
    void refuse(Request& r, RequestReject::Request kind, const RiskVerdict& v) {
        ++(v.reason == ExecReason::GAP_HALTED ? stats_.gap_rejects : stats_.risk_rejects);
        r.sent_at = now();
        if (gw_.listener_) gw_.listener_->on_request(index_, r, r.sent_at);
        reject_request(kind, r, v.reason, v.text, true);
    }

    // The drawdown kill switch, on the PnL of every symbol at the strategy's marks: on tripping, cancel
    // every open order
    void check_drawdown() {
        if (!risk_.limits().max_drawdown || !can_trade()) return;
        int64_t pnl = 0;
        for (const Leg& l : legs_) {
            if (l.known.position == 0) {
                pnl += l.known.cash - l.known.fees;
                continue;
            }
            const auto m = RiskGuard::mark(l.book);
            if (!m) return;  // a position that can't be marked yet
            pnl += l.known.pnl(*m);
        }
        if (risk_.update(pnl, now())) {
            stats_.risk_killed_at = risk_.killed_at();
            cancel_all(nullptr);
        }
        stats_.max_drawdown = risk_.max_drawdown_seen();
    }

    // Every open order not already being cancelled; with a symbol, that symbol's only
    void cancel_all(const std::string* symbol) {
        std::vector<OrderId> ids;
        for (const auto& [id, o] : known_orders_) {
            if (o.status != OrderStatus::PENDING_CANCEL && (!symbol || o.symbol == *symbol)) ids.push_back(id);
        }
        for (const auto& id : ids) cancel(id);
    }

    static RiskVerdict halt_verdict() {
        return RiskVerdict{ExecReason::GAP_HALTED, "trading halted: market data gap, waiting for a snapshot"};
    }

    // A symbol's feed lost updates: halt it until a snapshot makes its book valid again
    void on_feed_gap(uint32_t sym) {
        if (cfg_.gap == GapPolicy::CONTINUE) return;
        Leg& leg = legs_[sym];
        if (!leg.halted) {
            leg.halted = true;
            leg.halted_since = now();
            ++stats_.gap_halts;
        }
        if (can_trade()) cancel_all(&gw_.names_[sym]);
    }

    // The symbol's book is valid again: lift the halt; under flatten, first close its position at market
    void resume(uint32_t sym) {
        Leg& leg = legs_[sym];
        leg.halted = false;
        stats_.halted_ns += now() - leg.halted_since;
        const Quantity pos = leg.known.position;
        if (cfg_.gap != GapPolicy::FLATTEN || pos == 0 || !can_trade()) return;
        flattening_ = true;  // the risk system's own order: the RiskGuard doesn't check it
        send_on(sym, pos > 0 ? Side::SELL : Side::BUY, OrderType::MARKET, 0, pos > 0 ? pos : -pos,
                StpPolicy::CANCEL_MAKER);
        flattening_ = false;
        ++stats_.flatten_orders;
    }

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
        const uint64_t arrive = entry_.arrival(r.sent_at, gw_.latency_scale(Channel::ENTRY));
        if (gw_.listener_) {
            gw_.listener_->on_request(index_, r, arrive);
        }
        gw_.queue_.push(arrive, EventClass::ORDER_ARRIVE, SimEvent{index_, std::move(r)});
    }

    // local: refused before the latency pipe (RiskGuard, gap halt), delivered now without latency
    void to_strategy(const ExecReport& r, bool local = false) {
        const uint64_t deliver = local ? now() : ack_.arrival(now(), gw_.latency_scale(Channel::ACK));
        if (gw_.listener_) {
            gw_.listener_->on_report(index_, r, deliver);
        }
        gw_.queue_.push(deliver, EventClass::STRATEGY_EXEC, SimEvent{index_, r});
    }

    void reject_request(RequestReject::Request request, const Request& r, ExecReason reason, std::string text,
                        bool local = false) {
        RequestReject rr;
        rr.request = request;
        rr.order_id = r.order_id;
        rr.symbol = gw_.names_[r.symbol];
        rr.reason = reason;
        rr.text = std::move(text);
        rr.exchange_time = now();
        const uint64_t deliver = local ? now() : ack_.arrival(now(), gw_.latency_scale(Channel::ACK));
        if (gw_.listener_) {
            gw_.listener_->on_request_reject(index_, rr, deliver);
        }
        gw_.queue_.push(deliver, EventClass::STRATEGY_EXEC, SimEvent{index_, std::move(rr)});
    }

    // A new order refused before the engine: one REJECTED report, as the engine would send (seq 0: it
    // isn't one of the engine's reports)
    void reject_order(const Request& r, ExecReason reason, std::string text, bool local = false) {
        ExecReport rep;
        rep.timestamp = now();
        rep.type = ExecType::REJECTED;
        rep.reason = reason;
        rep.text = std::move(text);
        rep.order_id = r.order_id;
        rep.owner_id = cfg_.owner;
        rep.symbol = gw_.names_[r.symbol];
        rep.side = r.side;
        rep.order_type = r.order_type;
        rep.price = r.price;
        rep.quantity = r.quantity;
        to_strategy(rep, local);
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
    LatencyChannel feed_;
    LatencyChannel entry_;
    LatencyChannel ack_;
    std::vector<Leg> legs_;  // per symbol, the primary first

    // What the strategy knows (besides its books and accounts)
    std::map<OrderId, OpenOrder> known_orders_;
    uint64_t order_seq_ = 0;
    uint64_t timer_seq_ = 0;
    std::unordered_set<uint64_t> cancelled_timers_;

    // What the exchange knows (besides the accounts and open orders per symbol); the request limits are
    // the connection's
    std::deque<uint64_t> recent_;  // arrival times of orders in the last 10 s
    uint64_t day_ = 0;
    uint64_t today_ = 0;
    uint64_t minute_ = 0;
    uint64_t weight_used_ = 0;  // request weight charged in the current minute
    const Request* amending_ = nullptr;  // the amend being processed

    RiskGuard risk_;
    bool flattening_ = false;  // sending a flatten order
    SessionStats stats_;
};

SimGateway::SimGateway(MatchingEngine& engine, SimQueue& queue, std::string symbol, int64_t tick_size,
                       int64_t lot_size, uint64_t seed)
    : SimGateway(engine, queue, std::vector<SymbolInfo>{SymbolInfo{std::move(symbol), tick_size, lot_size}}, seed) {}

SimGateway::SimGateway(MatchingEngine& engine, SimQueue& queue, std::vector<SymbolInfo> symbols, uint64_t seed)
    : engine_(engine), queue_(queue), symbols_(std::move(symbols)), seed_(seed) {
    if (symbols_.empty()) {
        throw std::invalid_argument("SimGateway needs at least one symbol");
    }
    for (uint32_t i = 0; i < symbols_.size(); ++i) {
        const SymbolInfo& s = symbols_[i];
        if (s.tick_size <= 0 || s.lot_size <= 0) {
            throw std::invalid_argument("SimGateway: symbol '" + s.name + "' needs positive tick and lot sizes");
        }
        if (!by_symbol_.emplace(s.name, i).second) {
            throw std::invalid_argument("SimGateway: duplicate symbol '" + s.name + "'");
        }
        names_.push_back(s.name);
    }
    engine_.register_exec_report_callback([this](const ExecReport& r) { on_engine_report(r); });
}

SimGateway::~SimGateway() { engine_.register_exec_report_callback(nullptr); }

uint32_t SimGateway::symbol_index(const std::string& name) const {
    if (symbols_.size() == 1 && name == names_[0]) return 0;
    auto it = by_symbol_.find(name);
    if (it == by_symbol_.end()) {
        throw std::invalid_argument("unknown symbol '" + name + "'");
    }
    return it->second;
}

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

void SimGateway::set_latency_regime(const LatencyRegimeConfig& config) {
    regime_ = config.enabled() ? std::make_unique<LatencyRegime>(config) : nullptr;
}

void SimGateway::count_messages(const Batch& batch) {
    if (regime_ && (batch.kind == BatchKind::DEPTH || batch.kind == BatchKind::TRADE)) {
        regime_->record(now(), batch.size());
    }
}

double SimGateway::latency_scale(Channel channel) {
    if (!regime_) return 1.0;
    const LatencyRegimeConfig& c = regime_->config();
    const bool on = channel == Channel::FEED ? c.feed : channel == Channel::ENTRY ? c.entry : c.ack;
    return on ? regime_->multiplier(now()) : 1.0;
}

void SimGateway::publish(const Batch& batch) {
    if (batch.symbol >= symbols_.size()) {
        throw std::invalid_argument("SimGateway::publish: symbol index out of range");
    }
    count_messages(batch);
    for (auto& s : sessions_) {
        s->publish(batch);
    }
}

void SimGateway::observe(const Batch& batch) {
    if (batch.symbol >= symbols_.size()) {
        throw std::invalid_argument("SimGateway::observe: symbol index out of range");
    }
    count_messages(batch);
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

const Account& SimGateway::exchange_account(uint32_t session, uint32_t symbol) const {
    return sessions_.at(session)->exchange_account(symbol);
}

const Account& SimGateway::strategy_account(uint32_t session, uint32_t symbol) const {
    return sessions_.at(session)->strategy_account(symbol);
}

SessionStats SimGateway::stats(uint32_t session) const { return sessions_.at(session)->stats(); }

Context& SimGateway::context(uint32_t session) { return *sessions_.at(session); }

const std::string& SimGateway::owner(uint32_t session) const { return sessions_.at(session)->owner(); }

const Strategy& SimGateway::strategy(uint32_t session) const { return sessions_.at(session)->strategy(); }

size_t SimGateway::exchange_open_orders(uint32_t session) const { return sessions_.at(session)->exchange_open_orders(); }

size_t SimGateway::exchange_open_orders(uint32_t session, uint32_t symbol) const {
    return sessions_.at(session)->exchange_open_orders(symbol);
}

} // namespace sim
