// Gateway, strategy interface and accounts (P3.10), driven by a minimal event loop over the synthesizer.
#include <doctest.h>
#include "gateway.hpp"
#include "synthesizer.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace sim;

namespace {

constexpr int64_t kTick = 1000000;  // 0.01
constexpr int64_t kLot = 100000;    // 0.001
const std::string kSymbol = "TESTUSDT";
constexpr uint64_t kMs = 1000000;
constexpr uint8_t BID = 0, ASK = 1;
constexpr uint8_t AGGR_BUY = 0;

std::shared_ptr<const LatencyModel> constant_ms(double ms) { return std::make_shared<ConstantLatency>(ms * 1e6); }

SessionConfig session(const std::string& owner, double latency_ms, GatewayLimits limits = {}) {
    SessionConfig c;
    c.owner = owner;
    c.feed = c.entry = c.ack = constant_ms(latency_ms);
    c.fees = FeeSchedule{100000, 100000};  // 0.1% maker and taker
    c.limits = limits;
    return c;
}

// A strategy assembled from lambdas; logs every callback as "<ms> <what>"
struct Script : Strategy {
    std::function<void(Context&)> start;
    std::function<void(Context&, const ObservedBook&)> book;
    std::function<void(Context&, const ExecReport&)> exec;
    std::function<void(Context&, const RequestReject&)> reject;
    std::function<void(Context&, uint64_t)> timer;
    std::vector<std::string> log;

    std::string name() const override { return "script"; }
    static std::string ms(const Context& ctx) { return std::to_string(ctx.now() / kMs); }

    void on_start(Context& ctx) override {
        if (start) start(ctx);
    }
    void on_book(Context& ctx, const ObservedBook& b) override {
        if (book) book(ctx, b);
    }
    void on_trade(Context& ctx, const TradeEvent& t) override {
        log.push_back(ms(ctx) + " trade " + format_fixed(t.qty) + "@" + format_fixed(t.price));
    }
    void on_exec(Context& ctx, const ExecReport& r) override {
        std::string what = ms(ctx) + " " + execTypeToString(r.type);
        if (r.reason != ExecReason::NONE) what += "(" + execReasonToString(r.reason) + ")";
        if (r.type == ExecType::FILL) what += " " + format_fixed(r.last_qty) + "@" + format_fixed(r.last_px) + " t=" +
                                              std::to_string(r.timestamp / kMs);
        log.push_back(what);
        if (exec) exec(ctx, r);
    }
    void on_reject(Context& ctx, const RequestReject& r) override {
        log.push_back(ms(ctx) + " reject " + request_name(r.request) +
                      (r.reason != ExecReason::NONE ? "(" + execReasonToString(r.reason) + ")" : ""));
        if (reject) reject(ctx, r);
    }
    void on_timer(Context& ctx, uint64_t id) override {
        log.push_back(ms(ctx) + " timer " + std::to_string(id));
        if (timer) timer(ctx, id);
    }
    void on_gap(Context& ctx) override { log.push_back(ms(ctx) + " gap"); }
};

// The engine, synthesizer and gateway with a minimal loop: what P3.11's simulator does, without the
// replayer, windows or outputs
struct Harness {
    std::shared_ptr<SimClock> clock = std::make_shared<SimClock>(0);
    MatchingEngine engine{clock, EngineOptions{false, false}};
    SimQueue queue;
    std::unique_ptr<QueueModel> model = make_queue_model("cancel_from_back");
    Rng rng{1};
    std::unique_ptr<Synthesizer> synth;
    std::unique_ptr<SimGateway> gw;
    std::deque<std::vector<bse::Record>> store;

    Harness() {
        SymbolSpec spec;
        spec.symbol = kSymbol;
        spec.tick_size = kTick;
        spec.lot_size = kLot;
        spec.min_qty = kLot;
        engine.set_symbol_spec(spec);
        synth = std::make_unique<Synthesizer>(engine, *model, rng, SynthesizerOptions{kSymbol, kTick, kLot});
        gw = std::make_unique<SimGateway>(engine, queue, kSymbol, kTick, kLot, 42);
    }

    void data(BatchKind kind, std::vector<bse::Record> records) {
        store.push_back(std::move(records));
        const auto& r = store.back();
        queue.push(static_cast<uint64_t>(r.front().ts_local), EventClass::MARKET_DATA,
                   SimEvent{0, Batch{kind, r.data(), r.data() + r.size(), 0}});
    }

    static bse::Record rec(uint64_t ms, uint8_t kind, uint8_t side, int64_t px, int64_t qty, uint16_t flags = 0) {
        const auto ts = static_cast<int64_t>(ms * kMs);
        return bse::Record{ts, ts, px, qty, 1, kind, side, flags};
    }

    void snapshot(uint64_t ms, std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels) {
        std::vector<bse::Record> r{rec(ms, bse::SNAP_BEGIN, 0, 0, 0, bse::FROM_SNAPSHOT)};
        for (auto [side, px, qty] : levels) r.push_back(rec(ms, bse::SNAP_LEVEL, side, px, qty, bse::FROM_SNAPSHOT));
        r.push_back(rec(ms, bse::SNAP_END, 0, 0, 0, bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH));
        data(BatchKind::SNAPSHOT, std::move(r));
    }

    void depth(uint64_t ms, std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels) {
        std::vector<bse::Record> r;
        for (auto [side, px, qty] : levels) r.push_back(rec(ms, bse::DELTA, side, px, qty));
        r.back().flags = bse::LAST_IN_BATCH;
        data(BatchKind::DEPTH, std::move(r));
    }

    void trade(uint64_t ms, uint8_t aggressor, int64_t px, int64_t lots) {
        data(BatchKind::TRADE, {rec(ms, bse::TRADE, aggressor, px, lots)});
    }

    // Processes every event up to and including time ms
    void run_until(uint64_t ms) {
        while (!queue.empty() && queue.top().time <= ms * kMs) {
            auto e = queue.pop();
            clock->set(e.time);
            if (e.cls == EventClass::MARKET_DATA) {
                const Batch& b = std::get<Batch>(e.payload.body);
                synth->apply(b);
                gw->publish(b);
            } else {
                gw->dispatch(e);
            }
        }
        if (clock->now_ns() < ms * kMs) clock->set(ms * kMs);
    }
};

std::string lots(int64_t n) { return format_fixed(n * kLot); }
std::string px(int64_t ticks) { return format_fixed(ticks * kTick); }

} // namespace

TEST_SUITE("gateway") {

TEST_CASE("accounts: fills move position and cash; pnl at a mark") {
    Account a;
    ExecReport f;
    f.type = ExecType::FILL;
    f.side = Side::BUY;
    f.last_px = 100 * kFixedScale;
    f.last_qty = 2 * kFixedScale / 1000;  // 0.002
    f.fee = 20000;                        // 0.0002
    f.liquidity = Liquidity::MAKER;
    a.apply_fill(f);
    f.side = Side::SELL;
    f.last_px = 101 * kFixedScale;
    f.last_qty = kFixedScale / 1000;      // 0.001
    f.fee = 10100;
    f.liquidity = Liquidity::TAKER;
    a.apply_fill(f);
    CHECK(a.position == 100000);          // 0.001
    CHECK(a.cash == -20000000 + 10100000);  // -0.2 + 0.101
    CHECK(a.fees == 30100);
    CHECK(a.base_volume == 300000);
    CHECK(a.quote_volume == 30100000);
    CHECK(a.fills == 2);
    CHECK(a.maker_fills == 1);
    CHECK(a.taker_fills == 1);
    // at 102: -0.099 + 0.102 - 0.000301 = 0.002699
    CHECK(a.pnl(102 * kFixedScale) == 269900);
    Account short_one;
    short_one.position = -100000;
    short_one.cash = 10000000;
    CHECK(short_one.pnl(90 * kFixedScale) == 1000000);  // 0.1 - 0.09
}

TEST_CASE("golden (e): with 5 ms latency a stale ask is picked off; with none it is cancelled in time") {
    // t=0 snapshot: bid 100.00 x 5, ask 100.10 x 2. The strategy joins the ask with 1 lot.
    // t=20 the asks at 100.10 are pulled and 3 lots appear at 100.20: our ask is alone and stale.
    // t=22 a buyer lifts the best ask in the data (100.20): it would have hit our cheaper 100.10 first.
    // The strategy cancels when it sees its ask is below the market.
    auto scenario = [](double latency_ms, Harness& h, Script& s) {
        struct State {
            bool sent = false;
            OrderId id;
            bool cancelled = false;
        };
        auto st = std::make_shared<State>();
        s.book = [st](Context& ctx, const ObservedBook& b) {
            if (!b.valid()) return;
            if (!st->sent) {
                st->id = ctx.send(Side::SELL, OrderType::LIMIT, b.best_ask()->price, kLot);
                st->sent = true;
            } else if (!st->cancelled && ctx.open_orders().count(st->id) &&
                       b.best_ask()->price > ctx.open_orders().at(st->id).price) {
                ctx.cancel(st->id);
                st->cancelled = true;
            }
        };
        h.gw->add_session(s, session("S1", latency_ms));
        h.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
        h.depth(20, {{ASK, 10010, 0}, {ASK, 10020, 3}});
        h.trade(22, AGGR_BUY, 10020, 1);
        h.gw->start();
    };

    SUBCASE("5 ms feed, entry and ack") {
        Harness h;
        Script s;
        scenario(5, h, s);
        h.run_until(24);
        // Filled at 22 at the exchange; the strategy doesn't know yet
        CHECK(h.gw->exchange_account(0).position == -kLot);
        CHECK(h.gw->strategy_account(0).position == 0);
        CHECK(h.gw->context(0).open_orders().size() == 1);
        h.run_until(100);
        const std::vector<std::string> expected{
            "15 ack",                                  // sent at 5 (sees the snapshot), arrives at 10
            "27 trade " + lots(1) + "@" + px(10020),   // the data trade, seen 5 ms late
            "27 fill " + lots(1) + "@" + px(10010) + " t=22",  // picked off at 22, learned at 27
            "35 reject cancel",                        // cancel sent at 25 (on seeing t=20), arrived at 30: too late
        };
        CHECK(s.log == expected);
        CHECK(h.gw->strategy_account(0).position == -kLot);
        CHECK(h.gw->strategy_account(0).cash == 10010000);  // 0.001 x 100.10
        CHECK(h.gw->strategy_account(0).fees == 10010);     // 0.1% maker
        CHECK(h.gw->strategy_account(0).maker_fills == 1);
        CHECK(h.gw->context(0).open_orders().empty());
        // The data trade took 1 lot of real liquidity at 100.20: it comes off the synthetic queue there
        CHECK(h.synth->synthetic_qty(Side::SELL, 10020 * kTick) == 2 * kLot);
        auto st = h.gw->stats(0);
        CHECK(st.orders_sent == 1);
        CHECK(st.cancels_sent == 1);
        CHECK(st.request_rejects == 1);
    }
    SUBCASE("zero latency") {
        Harness h;
        Script s;
        scenario(0, h, s);
        h.run_until(100);
        const std::vector<std::string> expected{
            "0 ack",
            "20 cancelled(user)",
            "22 trade " + lots(1) + "@" + px(10020),
        };
        CHECK(s.log == expected);
        CHECK(h.gw->exchange_account(0).position == 0);
        CHECK(h.synth->synthetic_qty(Side::SELL, 10020 * kTick) == 2 * kLot);  // the tape took it from the data's own
    }
}

TEST_CASE("orders per 10 s: the excess is rejected at the exchange, and the window slides") {
    Harness h;
    Script s;
    GatewayLimits limits;
    limits.orders_per_10s = 3;
    s.start = [](Context& ctx) {
        for (int i = 0; i < 5; ++i) ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, kLot);
        ctx.set_timer(10000 * kMs);
    };
    s.timer = [](Context& ctx, uint64_t) { ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, kLot); };
    h.gw->add_session(s, session("S1", 1, limits));
    h.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    h.gw->start();
    h.run_until(20000);
    const std::vector<std::string> expected{"2 ack", "2 ack", "2 ack", "2 rejected(rate_limit)",
                                            "2 rejected(rate_limit)", "10000 timer 1", "10002 ack"};
    CHECK(s.log == expected);
    CHECK(h.gw->stats(0).rate_limited == 2);
    CHECK(h.gw->context(0).open_orders().size() == 4);
}

TEST_CASE("request weight per minute: every request type pays, the excess is refused, the next minute resets") {
    Harness h;
    Script s;
    GatewayLimits limits;
    limits.request_weight_per_min = 10;  // new 1, cancel 1, replace 1, amend 4
    std::vector<OrderId> ids;
    s.start = [&](Context& ctx) {
        for (int i = 0; i < 4; ++i) ids.push_back(ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, 2 * kLot));
        ctx.set_timer(10 * kMs);
    };
    s.timer = [&](Context& ctx, uint64_t id) {
        if (id == 1) {
            ctx.amend(ids[0], kLot);  // 4 + 4 = 8
            ctx.cancel(ids[1]);       // 9
            ctx.cancel(ids[2]);       // 10
            ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, kLot);  // 11: refused
            ctx.cancel(ids[3]);                                           // refused
            ctx.set_timer(59990 * kMs);  // arrives in the next minute
        } else {
            ctx.cancel(ids[3]);
        }
    };
    h.gw->add_session(s, session("S1", 1, limits));
    h.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    h.gw->start();
    h.run_until(61000);
    const std::vector<std::string> expected{
        "2 ack", "2 ack", "2 ack", "2 ack", "10 timer 1", "12 amended", "12 cancelled(user)", "12 cancelled(user)",
        "12 rejected(rate_limit)", "12 reject cancel(rate_limit)", "60000 timer 2", "60002 cancelled(user)"};
    CHECK(s.log == expected);
    CHECK(h.gw->stats(0).weight_limited == 2);
    CHECK(h.gw->stats(0).rate_limited == 0);
    CHECK(h.gw->context(0).open_orders().size() == 1);
}

TEST_CASE("max open orders; a cancel frees a slot") {
    Harness h;
    Script s;
    GatewayLimits limits;
    limits.max_open_orders = 2;
    std::vector<OrderId> ids;
    s.start = [&](Context& ctx) {
        for (int i = 0; i < 3; ++i) ids.push_back(ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, kLot));
        ctx.cancel(ids[0]);
        ids.push_back(ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, kLot));
    };
    h.gw->add_session(s, session("S1", 0, limits));
    h.gw->start();
    h.run_until(10);
    const std::vector<std::string> expected{"0 ack", "0 ack", "0 rejected(max_open_orders)", "0 cancelled(user)",
                                            "0 ack"};
    CHECK(s.log == expected);
    CHECK(ids[0] == "S1-1");
    CHECK(ids[3] == "S1-4");
    CHECK(h.gw->stats(0).open_limited == 1);
}

TEST_CASE("what the strategy knows: pending until the report arrives; engine rejects remove the order") {
    Harness h;
    Script s;
    OrderId good, bad;
    s.start = [&](Context& ctx) {
        good = ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, 2 * kLot);
        bad = ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick + 1, kLot);  // off the tick grid
    };
    h.gw->add_session(s, session("S1", 3));
    h.gw->start();
    auto& ctx = h.gw->context(0);
    REQUIRE(ctx.open_orders().size() == 2);
    CHECK(ctx.open_orders().at(good).status == OrderStatus::PENDING_NEW);
    CHECK(ctx.open_orders().at(good).leaves == 2 * kLot);
    h.run_until(5);  // arrived at 3, reports not back yet
    CHECK(ctx.open_orders().at(good).status == OrderStatus::PENDING_NEW);
    h.run_until(6);
    CHECK(ctx.open_orders().size() == 1);
    CHECK(ctx.open_orders().at(good).status == OrderStatus::NEW);
    CHECK(s.log == std::vector<std::string>{"6 ack", "6 rejected(price_tick)"});
}

TEST_CASE("amend, replace and cancel: accepted, refused, and the order view follows") {
    Harness h;
    Script s;
    OrderId a;
    h.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    s.start = [&](Context& ctx) { a = ctx.send(Side::BUY, OrderType::LIMIT, 10000 * kTick, 3 * kLot); };
    h.gw->add_session(s, session("S1", 1));
    h.gw->start();
    h.run_until(5);
    auto& ctx = h.gw->context(0);
    REQUIRE(ctx.open_orders().at(a).status == OrderStatus::NEW);

    ctx.amend(a, 2 * kLot);                     // smaller: keeps its place behind the 5 lots
    ctx.amend(a, 2 * kLot, 10000 * kTick + 7);  // off the grid: refused, order unchanged
    ctx.cancel("S1-99");                        // unknown
    h.run_until(10);
    CHECK(ctx.open_orders().at(a).quantity == 2 * kLot);
    CHECK(ctx.open_orders().at(a).status == OrderStatus::NEW);
    {
        const PriceLevel* level = h.engine.find_level(kSymbol, Side::BUY, 10000 * kTick);
        REQUIRE(level);
        CHECK(level->tail->order_id == a);  // still behind the synthetic 5
    }

    ctx.replace(a, 9990 * kTick, 4 * kLot);     // new price and size, same ID
    CHECK(ctx.open_orders().at(a).status == OrderStatus::PENDING_REPLACE);
    h.run_until(15);
    CHECK(ctx.open_orders().at(a).price == 9990 * kTick);
    CHECK(ctx.open_orders().at(a).quantity == 4 * kLot);
    CHECK(ctx.open_orders().at(a).status == OrderStatus::NEW);

    ctx.cancel(a);
    CHECK(ctx.open_orders().at(a).status == OrderStatus::PENDING_CANCEL);
    h.run_until(20);
    CHECK(ctx.open_orders().empty());
    ctx.replace(a, 9990 * kTick, kLot);         // the order is gone: no new order
    ctx.amend(a, kLot);
    h.run_until(25);
    CHECK(ctx.open_orders().empty());
    const std::vector<std::string> expected{
        "2 ack",
        "7 amended",
        "7 reject amend(price_tick)",
        "7 reject cancel",
        "12 cancelled(replaced)",
        "12 ack",
        "17 cancelled(user)",
        "22 reject replace",
        "22 reject amend",
    };
    CHECK(s.log == expected);
    CHECK(h.gw->stats(0).request_rejects == 4);
}

TEST_CASE("timers fire in order; a cancelled timer doesn't fire") {
    Harness h;
    Script s;
    s.start = [](Context& ctx) {
        ctx.set_timer(30 * kMs);
        const uint64_t t2 = ctx.set_timer(10 * kMs);
        ctx.set_timer(20 * kMs);
        ctx.cancel_timer(t2);
    };
    h.gw->add_session(s, session("S1", 1));
    h.gw->start();
    h.run_until(100);
    CHECK(s.log == std::vector<std::string>{"20 timer 3", "30 timer 1"});
    CHECK(h.gw->stats(0).timers == 2);
}

TEST_CASE("two strategies trade with each other; each sees only its own reports and pays its own fees") {
    Harness h;
    Script a, b;
    h.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 5}});
    a.start = [](Context& ctx) { ctx.send(Side::SELL, OrderType::LIMIT, 10005 * kTick, 2 * kLot); };
    b.timer = [](Context& ctx, uint64_t) { ctx.send(Side::BUY, OrderType::IOC, 10005 * kTick, kLot); };
    b.start = [](Context& ctx) { ctx.set_timer(10 * kMs); };
    auto cfg_b = session("S2", 1);
    cfg_b.fees = FeeSchedule{0, 50000};  // 0.05% taker
    h.gw->add_session(a, session("S1", 1));
    h.gw->add_session(b, cfg_b);
    h.gw->start();
    h.run_until(50);
    CHECK(a.log == std::vector<std::string>{"2 ack", "12 fill " + lots(1) + "@" + px(10005) + " t=11"});
    CHECK(b.log == std::vector<std::string>{"10 timer 1", "12 ack", "12 fill " + lots(1) + "@" + px(10005) + " t=11"});
    CHECK(h.gw->exchange_account(0).position == -kLot);
    CHECK(h.gw->exchange_account(1).position == kLot);
    CHECK(h.gw->exchange_account(0).fees == 10005);  // 0.1% of 0.10005
    CHECK(h.gw->exchange_account(1).fees == 5003);   // 0.05%, rounded half up from 5002.5
    CHECK(h.gw->exchange_account(1).taker_fills == 1);
    CHECK(h.gw->context(0).open_orders().begin()->second.status == OrderStatus::PARTIALLY_FILLED);
}

TEST_CASE("requests on one connection never overtake: every cancel finds its order, despite random latency") {
    Harness h;
    Script s;
    auto cfg = session("S1", 1, GatewayLimits{0, 0, 0});  // 300 orders in 30 ms: no rate limits here
    cfg.entry = std::make_shared<NormalLatency>(2e6, 3e6);  // 2 ms +- 3 ms: samples would reorder freely
    s.start = [](Context& ctx) { ctx.set_timer(kMs); };
    int rounds = 0;
    s.timer = [&](Context& ctx, uint64_t) {
        const OrderId id = ctx.send(Side::BUY, OrderType::LIMIT, 9000 * kTick, kLot);
        ctx.cancel(id);
        if (++rounds < 300) ctx.set_timer(100000);  // every 0.1 ms
    };
    h.gw->add_session(s, cfg);
    h.gw->start();
    h.run_until(10000);
    int acks = 0, cancels = 0, rejects = 0;
    for (const auto& line : s.log) {
        acks += line.find(" ack") != std::string::npos;
        cancels += line.find("cancelled(user)") != std::string::npos;
        rejects += line.find("reject") != std::string::npos;
    }
    CHECK(acks == 300);
    CHECK(cancels == 300);
    CHECK(rejects == 0);
    CHECK(h.gw->stats(0).held_back > 50);
    CHECK(h.gw->context(0).open_orders().empty());
}

TEST_CASE("configuration errors") {
    Harness h;
    Script s;
    CHECK_THROWS_AS(h.gw->add_session(s, session("MARKET", 1)), std::invalid_argument);
    CHECK_THROWS_AS(h.gw->add_session(s, session("", 1)), std::invalid_argument);
    auto no_feed = session("S1", 1);
    no_feed.feed = nullptr;
    CHECK_THROWS_AS(h.gw->add_session(s, no_feed), std::invalid_argument);
    h.gw->add_session(s, session("S1", 1));
    CHECK_THROWS_AS(h.gw->add_session(s, session("S1", 1)), std::invalid_argument);
}

} // TEST_SUITE
