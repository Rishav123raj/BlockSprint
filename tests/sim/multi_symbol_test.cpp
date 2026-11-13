// Multi-symbol runs (P6.7): two files merged by time, each strategy session with a book, accounts, open
// orders and a gap halt per symbol, the request limits shared, the recorder's symbol column. The expected
// logs are worked out by hand in the comments.
#include <doctest.h>
#include "recorder.hpp"
#include "sim_helpers.hpp"

#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace simtest;
using nlohmann::json;


namespace {

constexpr int64_t kTick = 1000000;  // 0.01

// A strategy made of lambdas that logs "<ms> <what> <symbol>"
struct Multi : Strategy {
    std::function<void(Multi&, Context&)> start;
    std::function<void(Multi&, Context&, const ObservedBook&)> book;
    std::function<void(Multi&, Context&)> gap;
    std::vector<std::string> log;
    bool quiet_books = false;  // don't log on_book

    std::string name() const override { return "multi"; }
    void note(const Context& ctx, const std::string& what) { log.push_back(std::to_string(ctx.now() / kMs) + " " + what); }

    void on_start(Context& ctx) override {
        if (start) start(*this, ctx);
    }
    void on_book(Context& ctx, const ObservedBook& b) override {
        if (!quiet_books) note(ctx, "book " + b.symbol());
        if (book) book(*this, ctx, b);
    }
    void on_trade(Context& ctx, const TradeEvent& t) override { note(ctx, "trade " + ctx.symbols()[t.symbol]); }
    void on_gap(Context& ctx) override {
        note(ctx, "gap");
        if (gap) gap(*this, ctx);
    }
    void on_exec(Context& ctx, const ExecReport& r) override {
        std::string what = execTypeToString(r.type) + " " + r.symbol;
        if (r.reason != ExecReason::NONE) what += "(" + execReasonToString(r.reason) + ")";
        if (r.type == ExecType::FILL) what += " " + format_fixed(r.last_qty) + "@" + format_fixed(r.last_px);
        note(ctx, what);
    }
};

// TESTUSDT: snapshot 0 (bid 100.00 x 5, ask 100.10 x 2), depth 10, a buyer at 20, depth 30
Stream test_stream() {
    Stream s;
    s.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    s.depth(10, {{BID, 10000, 6}});
    s.trade(20, AGGR_BUY, 10010, 1);
    s.depth(30, {{BID, 10000, 7}});
    return s;
}

// OTHERUSDT: snapshot 5 (bid 50.00 x 5, ask 50.10 x 5), depth 10 (the same time as TESTUSDT's), a gap at 25,
// the resync snapshot at 40
Stream other_stream() {
    Stream s;
    s.snapshot(5, {{BID, 5000, 5}, {ASK, 5010, 5}});
    s.depth(10, {{BID, 5000, 6}});
    s.gap(25);
    s.snapshot(40, {{BID, 5000, 5}, {ASK, 5010, 5}});
    return s;
}

struct Files {
    TempFile test, other;
    Files() {
        test_stream().write(test, "TESTUSDT");
        other_stream().write(other, "OTHERUSDT");
    }
    std::vector<std::string> paths() const { return {test.str(), other.str()}; }
};

// A log as one string, for failure messages
std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& x : v) out += "\n  " + x;
    return out;
}

SimulatorOptions window(uint64_t to_ms) {
    SimulatorOptions o;
    o.to_ns = to_ms * kMs;
    return o;
}

std::vector<std::string> lines_of(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

} // namespace

TEST_SUITE("multi_symbol") {

TEST_CASE("two files merge by time; equal times in queue order; each book its own") {
    // Both depth events at 10: TESTUSDT's was queued first (after its batch at 0; OTHERUSDT's after 5)
    Files f;
    Simulator sim(f.paths(), window(100));
    Multi s;
    std::vector<std::string> symbols;
    s.start = [&](Multi&, Context& ctx) { symbols = ctx.symbols(); };
    sim.add_strategy(s, session("S1", 1));
    sim.run();
    CHECK(symbols == std::vector<std::string>{"TESTUSDT", "OTHERUSDT"});
    // feed latency 1 ms
    const std::vector<std::string> expected{"1 book TESTUSDT", "6 book OTHERUSDT", "11 book TESTUSDT",
                                            "11 book OTHERUSDT", "21 trade TESTUSDT", "26 gap",
                                            "31 book TESTUSDT", "41 book OTHERUSDT"};
    CHECK_MESSAGE(s.log == expected, joined(s.log));
    CHECK(sim.stats().data_batches == 8);
    CHECK(sim.symbol_count() == 2);
    CHECK(sim.header(1).symbol == "OTHERUSDT");
    Context& ctx = sim.gateway().context(0);
    CHECK(ctx.symbol() == "TESTUSDT");
    CHECK(ctx.book().best_bid()->qty == 7 * kLot);
    CHECK(ctx.book("OTHERUSDT").best_bid()->price == 5000 * kTick);
    CHECK(ctx.book("OTHERUSDT").valid());
    CHECK(sim.synthesizer(1).stats().gaps == 1);
    CHECK_THROWS_AS(ctx.book("XUSDT"), std::invalid_argument);
    CHECK_THROWS_AS(Simulator(std::vector<std::string>{f.test.str(), f.test.str()}, SimulatorOptions{}),
                    std::invalid_argument);
}

TEST_CASE("fast-forward merges both files: the window starts with both books in sync") {
    Files f;
    SimulatorOptions o;
    o.from_ns = 12 * kMs;
    Simulator sim(f.paths(), o);
    Multi s;
    bool both = false;
    s.start = [&](Multi&, Context& ctx) {
        both = ctx.book("TESTUSDT").valid() && ctx.book("OTHERUSDT").valid() &&
               ctx.book("OTHERUSDT").best_bid()->qty == 6 * kLot && ctx.book().best_bid()->qty == 6 * kLot;
    };
    sim.add_strategy(s, session("S1", 1));
    sim.run();
    CHECK(both);
    CHECK(sim.stats().fast_forward_batches == 4);
    CHECK(sim.stats().data_batches == 4);
}

TEST_CASE("orders per symbol: books, accounts and open orders; a gap halts only its symbol") {
    // t=0: buy 2 lots of TESTUSDT at 100.10 (takes the ask) and bid 1 lot at 99.00.
    // t=6, OTHERUSDT's first book: sell 2 lots at 50.00 (takes the bid) and bid 1 lot at 49.00.
    // t=26, the OTHERUSDT gap: its bid is cancelled (halt); a new OTHERUSDT order is refused at once, a
    // TESTUSDT one goes through. t=41: the resync lifts the halt.
    Files f;
    Simulator sim(f.paths(), window(100));
    Multi s;
    s.quiet_books = true;
    bool other_sent = false;
    s.start = [](Multi&, Context& ctx) {
        ctx.send(Side::BUY, OrderType::LIMIT, 10010 * kTick, 2 * kLot);
        ctx.send("TESTUSDT", Side::BUY, OrderType::LIMIT, 9900 * kTick, kLot);
    };
    s.book = [&](Multi&, Context& ctx, const ObservedBook& b) {
        if (other_sent || b.symbol() != "OTHERUSDT") return;
        other_sent = true;
        ctx.send("OTHERUSDT", Side::SELL, OrderType::LIMIT, 5000 * kTick, 2 * kLot);
        ctx.send("OTHERUSDT", Side::BUY, OrderType::LIMIT, 4900 * kTick, kLot);
    };
    s.gap = [](Multi&, Context& ctx) {
        ctx.send("OTHERUSDT", Side::BUY, OrderType::LIMIT, 4900 * kTick, kLot);
        ctx.send("TESTUSDT", Side::BUY, OrderType::LIMIT, 9800 * kTick, kLot);
        CHECK_THROWS_AS(ctx.send("XUSDT", Side::BUY, OrderType::LIMIT, 100 * kTick, kLot), std::invalid_argument);
    };
    sim.add_strategy(s, session("S1", 1));
    sim.run();
    const std::vector<std::string> expected{
        "2 ack TESTUSDT", "2 fill TESTUSDT 0.002@100.1", "2 ack TESTUSDT",
        "8 ack OTHERUSDT", "8 fill OTHERUSDT 0.002@50.0", "8 ack OTHERUSDT",
        "21 trade TESTUSDT",
        "26 gap", "26 rejected OTHERUSDT(gap_halted)", "28 cancelled OTHERUSDT(user)", "28 ack TESTUSDT"};
    CHECK_MESSAGE(s.log == expected, joined(s.log));
    SimGateway& gw = sim.gateway();
    Context& ctx = gw.context(0);
    CHECK(ctx.account().position == 2 * kLot);
    CHECK(ctx.account("OTHERUSDT").position == -2 * kLot);
    CHECK(ctx.account("OTHERUSDT").cash == 10000000);  // 0.002 x 50.00 = 0.1
    CHECK(gw.exchange_account(0, 1).position == -2 * kLot);
    CHECK(gw.exchange_open_orders(0, 0) == 2);
    CHECK(gw.exchange_open_orders(0, 1) == 0);
    CHECK(gw.exchange_open_orders(0) == 2);
    for (const auto& [id, o] : ctx.open_orders()) CHECK(o.symbol == "TESTUSDT");
    CHECK(gw.stats(0).gap_halts == 1);
    CHECK(gw.stats(0).gap_rejects == 1);
    CHECK(gw.stats(0).halted_ns == 15 * kMs);  // 26 to the resync at 41
}

TEST_CASE("limits: position per symbol, open orders per symbol, order rates per connection") {
    Files f;
    SUBCASE("the RiskGuard's max position is per symbol") {
        Simulator sim(f.paths(), SimulatorOptions{});
        Multi s;
        s.quiet_books = true;
        s.start = [](Multi&, Context& ctx) {
            ctx.send(Side::BUY, OrderType::LIMIT, 9900 * kTick, 2 * kLot);
            ctx.send("OTHERUSDT", Side::BUY, OrderType::LIMIT, 4900 * kTick, 2 * kLot);  // another base asset
            ctx.send(Side::BUY, OrderType::LIMIT, 9900 * kTick, kLot);                 // 3 lots of TESTUSDT
        };
        SessionConfig c = session("S1", 1);
        c.risk.max_position = 2 * kLot;
        sim.add_strategy(s, c);
        sim.run();
        // then the OTHERUSDT gap halts it and cancels its bid
        const std::vector<std::string> expected{"0 rejected TESTUSDT(risk_position)", "2 ack TESTUSDT",
                                                "2 ack OTHERUSDT",
                                                "21 trade TESTUSDT", "26 gap", "28 cancelled OTHERUSDT(user)"};
        CHECK_MESSAGE(s.log == expected, joined(s.log));
    }
    SUBCASE("max open orders per symbol; orders per 10 s across symbols") {
        Simulator sim(f.paths(), SimulatorOptions{});
        Multi s;
        s.quiet_books = true;
        s.start = [](Multi&, Context& ctx) {
            ctx.send(Side::BUY, OrderType::LIMIT, 9900 * kTick, kLot);
            ctx.send("OTHERUSDT", Side::BUY, OrderType::LIMIT, 4900 * kTick, kLot);  // its own symbol's slot
            ctx.send(Side::BUY, OrderType::LIMIT, 9800 * kTick, kLot);              // TESTUSDT's second
            ctx.send("OTHERUSDT", Side::BUY, OrderType::LIMIT, 4800 * kTick, kLot);  // the 4th order in 10 s
        };
        SessionConfig c = session("S1", 1);
        c.limits.max_open_orders = 1;
        c.limits.orders_per_10s = 3;
        sim.add_strategy(s, c);
        sim.run();
        const std::vector<std::string> expected{"2 ack TESTUSDT", "2 ack OTHERUSDT",
                                                "2 rejected TESTUSDT(max_open_orders)",
                                                "2 rejected OTHERUSDT(rate_limit)",
                                                "21 trade TESTUSDT", "26 gap", "28 cancelled OTHERUSDT(user)"};
        CHECK_MESSAGE(s.log == expected, joined(s.log));
    }
}

TEST_CASE("the recorder: a symbol column after the owner, samples and positions per symbol, metadata") {
    Files f;
    TempPath out("");
    Simulator sim(f.paths(), SimulatorOptions{});
    Multi s;
    s.quiet_books = true;
    s.start = [](Multi&, Context& ctx) {
        ctx.send(Side::BUY, OrderType::LIMIT, 10010 * kTick, kLot);
        ctx.send("OTHERUSDT", Side::BUY, OrderType::LIMIT, 4900 * kTick, kLot);
    };
    SessionConfig c = session("S1", 1);
    c.gap = GapPolicy::CONTINUE;  // keep the OTHERUSDT bid through its gap
    sim.add_strategy(s, c);
    json meta;
    {
        ResultRecorder rec(sim, out.str(), 20 * kMs);
        sim.run();
        meta = rec.finish();
    }
    const auto orders = lines_of(out.path / "orders.csv");
    REQUIRE(orders.size() == 3);
    CHECK(orders[0] == "sent_ns,arrive_ns,owner,symbol,request,order_id,side,order_type,price,quantity,new_price,stp");
    CHECK(orders[1].find(",S1,TESTUSDT,new,S1-1,") != std::string::npos);
    CHECK(orders[2].find(",S1,OTHERUSDT,new,S1-2,") != std::string::npos);
    const auto reports = lines_of(out.path / "exec_reports.csv");
    CHECK(reports[0].rfind("exchange_ns,deliver_ns,owner,symbol,seq,", 0) == 0);
    CHECK(reports.back().find(",S1,OTHERUSDT,") != std::string::npos);
    CHECK(lines_of(out.path / "request_rejects.csv")[0] ==
          "exchange_ns,deliver_ns,owner,symbol,request,order_id,reason,text");
    // Samples at 0, 20, 40 (the end is 40 ms + 1 ns), one row per symbol each
    const auto bbo = lines_of(out.path / "bbo_samples.csv");
    REQUIRE(bbo.size() == 7);
    CHECK(bbo[1].rfind("0,S1,TESTUSDT,,,,,", 0) == 0);  // sampled before the data at 0
    CHECK(bbo[2].rfind("0,S1,OTHERUSDT,,,,,", 0) == 0);
    // At 20: the data's ask is 2 lots, the engine's 1 (the strategy took one at 1)
    CHECK_MESSAGE(bbo[3].rfind("20000000,S1,TESTUSDT,100.0,0.006,100.1,0.002,100.0,0.006,100.1,0.001,", 0) == 0, bbo[3]);
    CHECK(bbo[4].rfind("20000000,S1,OTHERUSDT,50.0,0.006,50.1,0.005,", 0) == 0);
    const auto pos = lines_of(out.path / "positions.csv");
    REQUIRE(pos.size() == 9);  // 3 samples and the end, 2 symbols each
    CHECK(pos[0].rfind("ts_ns,owner,symbol,position,", 0) == 0);
    CHECK(pos[7].rfind("40000001,S1,TESTUSDT,0.001,", 0) == 0);
    CHECK(pos[8].rfind("40000001,S1,OTHERUSDT,0.0,0.0,0.0,50.05,0.0,0,0,0,1,", 0) == 0);
    // Metadata: the primary's sections as before, every symbol's in "symbols", accounts per symbol
    CHECK(meta["data_header"]["symbol"] == "TESTUSDT");
    REQUIRE(meta["symbols"].size() == 2);
    CHECK(meta["symbols"][1]["symbol"] == "OTHERUSDT");
    CHECK(meta["symbols"][1]["synthesizer"]["gaps"] == 1);
    CHECK(meta["symbols"][1]["final_mark"] == "50.05");
    CHECK(meta["sessions"][0]["exchange_account"]["position"] == "0.001");
    CHECK(meta["sessions"][0]["accounts"]["OTHERUSDT"]["open_orders"] == 1);
    CHECK(meta["sessions"][0]["accounts"]["TESTUSDT"]["exchange_account"]["position"] == "0.001");
}

} // TEST_SUITE
