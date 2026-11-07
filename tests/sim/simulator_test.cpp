// The simulator loop (P3.11): fast-forward, window, warm-up, end of data, tie-breaking, sampling, determinism.
#include <doctest.h>
#include "sim_helpers.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace simtest;

namespace {

// Snapshot at 0 (bid 100.00 x 5, ask 100.10 x 2); depth every 10 ms to 100 (bid qty 5 + ms/10); a trade at 45
Stream ten_ms_stream() {
    Stream s;
    s.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    for (int64_t ms = 10; ms <= 100; ms += 10) {
        s.depth(ms, {{BID, 10000, 5 + ms / 10}});
        if (ms == 40) s.trade(45, AGGR_BUY, 10010, 1);
    }
    return s;
}

} // namespace

TEST_SUITE("simulator") {

TEST_CASE("fast-forward, window and warm-up: no trading until the warm-up ends, nothing after the end") {
    TempFile f;
    ten_ms_stream().write(f);
    SimulatorOptions opt;
    opt.from_ns = 30 * kMs;
    opt.to_ns = 80 * kMs;
    opt.warmup_ns = 20 * kMs;
    Simulator sim(f.str(), opt);

    Script s;
    bool sent = false;
    s.start = [](Script& self, Context& ctx) {
        // The book was fast-forwarded: in sync with the data up to t=20, no feed latency
        REQUIRE(ctx.book().valid());
        self.note(ctx, "start bid=" + std::to_string(ctx.book().best_bid()->qty / kLot) +
                           (ctx.can_trade() ? " trading" : " warm-up"));
        ctx.set_timer(10 * kMs);   // fires at 40, during the warm-up: timers are allowed
        ctx.set_timer(100 * kMs);  // would fire at 130: dropped
    };
    s.book = [&sent](Script& self, Context& ctx, const ObservedBook& b) {
        if (!ctx.can_trade()) {
            CHECK_THROWS_AS(ctx.send(Side::BUY, OrderType::LIMIT, 9900000000, kLot), std::logic_error);
            self.note(ctx, "book warm-up");
        } else if (!sent) {
            ctx.send(Side::BUY, OrderType::LIMIT, b.best_bid()->price - 1000000, kLot);
            sent = true;
            self.note(ctx, "book send");
        } else {
            self.note(ctx, "book");
        }
    };
    s.end = [](Script& self, Context& ctx) { self.note(ctx, "end open=" + std::to_string(ctx.open_orders().size())); };
    sim.add_strategy(s, session("S1", 1));
    std::vector<uint64_t> samples;
    sim.set_sampler(20 * kMs, [&](uint64_t t) {
        CHECK(sim.now() == t);
        samples.push_back(t / kMs);
    });
    sim.run();

    const std::vector<std::string> expected{
        "30 start bid=7 warm-up", "31 book warm-up", "40 timer 1", "41 book warm-up", "46 trade",
        "51 book send",           "53 ack",          "61 book",    "71 book",         "80 end open=1",
    };
    CHECK(s.log == expected);
    CHECK(samples == std::vector<uint64_t>{30, 50, 70});
    const SimStats& st = sim.stats();
    CHECK(st.start_ns == 30 * kMs);
    CHECK(st.trading_ns == 50 * kMs);
    CHECK(st.end_ns == 80 * kMs);
    CHECK(st.fast_forward_batches == 3);  // snapshot, depth 10, depth 20
    CHECK(st.data_batches == 6);          // depth 30, 40, trade 45, depth 50, 60, 70
    CHECK(st.data_left == 1);
    CHECK(st.dropped == 1);               // the timer at 130
    CHECK(st.events[static_cast<size_t>(EventClass::ORDER_ARRIVE)] == 1);
    CHECK(st.samples == 3);
    CHECK(sim.now() == 80 * kMs);
    CHECK_THROWS_AS(sim.run(), std::logic_error);
}

TEST_CASE("without a window the run covers the whole file and ends just after the last data") {
    TempFile f;
    ten_ms_stream().write(f);
    Simulator sim(f.str(), SimulatorOptions{});
    Script s;
    s.start = [](Script& self, Context& ctx) {
        self.note(ctx, std::string("start ") + (ctx.book().valid() ? "valid" : "empty"));
        ctx.set_timer(200 * kMs);
    };
    s.book = [](Script& self, Context& ctx, const ObservedBook&) { self.note(ctx, "book"); };
    s.end = [](Script& self, Context& ctx) { self.note(ctx, "end"); };
    sim.add_strategy(s, session("S1", 1));
    sim.run();
    CHECK(s.log.front() == "0 start empty");  // on_start comes before the first batch
    CHECK(s.log[1] == "1 book");
    CHECK(s.log.back() == "100 end");
    const SimStats& st = sim.stats();
    CHECK(st.start_ns == 0);
    CHECK(st.end_ns == 100 * kMs + 1);
    CHECK(st.fast_forward_batches == 0);
    CHECK(st.data_batches == 12);   // snapshot, 10 depth events, 1 trade
    CHECK(st.data_left == 0);
    CHECK(st.dropped == 2);         // the last batch's delivery at 101 and the timer at 200
}

TEST_CASE("bad windows and queue models are refused") {
    TempFile f;
    ten_ms_stream().write(f);
    SimulatorOptions opt;
    opt.from_ns = 50;
    opt.to_ns = 50;
    CHECK_THROWS_AS(Simulator(f.str(), opt), std::invalid_argument);
    opt.to_ns = 100;
    opt.warmup_ns = 50;
    CHECK_THROWS_AS(Simulator(f.str(), opt), std::invalid_argument);
    opt.warmup_ns = 0;
    opt.queue_model = "no_such_model";
    CHECK_THROWS(Simulator(f.str(), opt));
    SimulatorOptions ok;
    Simulator sim(f.str(), ok);
    CHECK_THROWS_AS(sim.set_sampler(0, [](uint64_t) {}), std::invalid_argument);
}

TEST_CASE("tie-breaking: an IOC arriving with the data that pulls its liquidity") {
    // The strategy sees the snapshot at 5 and sends an IOC for the 100.10 ask; it arrives at 10, when the
    // data pulls that ask. Data first: nothing to take. Orders first: filled.
    TempFile f;
    Stream s;
    s.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    s.depth(10, {{ASK, 10010, 0}, {ASK, 10020, 3}});
    s.depth(50, {{BID, 10000, 6}});
    s.write(f);

    auto run = [&](TieBreak tb) {
        SimulatorOptions opt;
        opt.tie_break = tb;
        Simulator sim(f.str(), opt);
        Script st;
        bool sent = false;
        st.book = [&sent](Script&, Context& ctx, const ObservedBook& b) {
            if (!sent && b.valid()) {
                ctx.send(Side::BUY, OrderType::IOC, b.best_ask()->price, kLot);
                sent = true;
            }
        };
        sim.add_strategy(st, session("S1", 5));
        sim.run();
        return std::make_pair(sim.gateway().exchange_account(0).position, st.log);
    };
    auto [data_first_pos, data_first_log] = run(TieBreak::DATA_FIRST);
    auto [orders_first_pos, orders_first_log] = run(TieBreak::ORDERS_FIRST);
    CHECK(data_first_pos == 0);
    CHECK(data_first_log == std::vector<std::string>{"15 ack", "15 cancelled"});
    CHECK(orders_first_pos == kLot);
    CHECK(orders_first_log == std::vector<std::string>{"15 ack", "15 fill"});
}

TEST_CASE("the same seed gives the same run; another seed gives other latencies") {
    // 2000 depth events (1 ms apart) with a moving bid and trades; the strategy keeps a bid at the best bid
    TempFile f;
    Stream s;
    s.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 5}});
    Rng data_rng(99);
    int64_t bid = 10000;
    for (int64_t ms = 1; ms <= 2000; ++ms) {
        const int64_t move = static_cast<int64_t>(data_rng.next() % 3) - 1;
        if (move != 0) {
            const int64_t nb = bid + move;
            s.depth(ms, {{BID, bid, 0}, {BID, nb, 1 + static_cast<int64_t>(data_rng.next() % 9)},
                         {ASK, bid + 1 + move, 0}, {ASK, nb + 10, 5}});
            bid = nb;
        } else {
            s.trade(ms, data_rng.next() % 2 ? AGGR_BUY : 1, bid, 1 + static_cast<int64_t>(data_rng.next() % 4));
        }
    }
    s.write(f);

    auto run = [&](uint64_t seed) {
        SimulatorOptions opt;
        opt.seed = seed;
        opt.queue_model = "prob_power:2";
        Simulator sim(f.str(), opt);
        Script st;
        OrderId live;
        st.book = [&live](Script&, Context& ctx, const ObservedBook& b) {
            if (!b.valid() || !b.best_bid()) return;
            const auto& open = ctx.open_orders();
            auto it = open.find(live);
            if (it == open.end()) {
                live = ctx.send(Side::BUY, OrderType::LIMIT, b.best_bid()->price, kLot);
            } else if (it->second.status == OrderStatus::NEW && it->second.price != b.best_bid()->price) {
                ctx.cancel(live);
            }
        };
        SessionConfig c;
        c.owner = "S1";
        c.feed = std::make_shared<LogNormalLatency>(1e6, 0.5);
        c.entry = std::make_shared<LogNormalLatency>(2e6, 0.5);
        c.ack = std::make_shared<LogNormalLatency>(1e6, 0.5);
        sim.add_strategy(st, c);
        struct Trace : GatewayListener {
            std::vector<std::string> lines;
            void on_report(uint32_t, const ExecReport& r, uint64_t deliver_at) override {
                lines.push_back(std::to_string(r.timestamp) + " " + r.order_id + " " + execTypeToString(r.type) + " " +
                                std::to_string(deliver_at));
            }
        } tracer;
        sim.gateway().set_listener(&tracer);
        sim.run();
        sim.gateway().set_listener(nullptr);
        const std::vector<std::string>& trace = tracer.lines;
        const Account& a = sim.gateway().exchange_account(0);
        return std::make_tuple(trace, a.position, a.cash, a.fills);
    };
    const auto a = run(7);
    const auto b = run(7);
    const auto c = run(8);
    CHECK(std::get<0>(a).size() > 100);
    CHECK(std::get<3>(a) > 0);
    CHECK(a == b);
    CHECK(std::get<0>(a) != std::get<0>(c));
}

} // TEST_SUITE("simulator")
