// Strategy registry and parameters (P3.14); the reference strategies (P3.15).
#include <doctest.h>
#include "sim_helpers.hpp"
#include "strategies/params.hpp"
#include "strategies/passive_quoter.hpp"
#include "strategies/registry.hpp"
#include "strategies/twap_taker.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace simtest;
using nlohmann::json;

namespace {

constexpr int64_t kPx = 1000000;  // one tick (0.01) in 1e-8 units
constexpr uint8_t AGGR_SELL = 1;

// Requests as "<ms> <type> <side> <price ticks> <lots>"
struct RequestLog : GatewayListener {
    std::vector<std::string> lines;
    void on_request(uint32_t, const Request& r, uint64_t) override {
        static const char* types[] = {"new", "cancel", "amend", "replace"};
        lines.push_back(std::to_string(r.sent_at / kMs) + " " + types[static_cast<int>(r.type)] + " " +
                        (r.side == Side::BUY ? "buy" : "sell") + " " + std::to_string(r.price / kPx) + " " +
                        std::to_string(r.quantity / kLot));
    }
};

std::string error_of(const std::string& name, const json& params) {
    try {
        make_strategy(name, params);
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return "no error";
}

} // namespace

TEST_SUITE("strategies") {

TEST_CASE("the registry builds strategies by name and refuses unknown names and parameters") {
    auto s = make_strategy("null", json::object());
    REQUIRE(s);
    CHECK(s->name() == "null");
    CHECK(make_strategy("null", json())->name() == "null");  // no params at all
    CHECK(error_of("nope", json::object()).find("unknown strategy 'nope' (null") != std::string::npos);
    CHECK(error_of("null", json{{"size", "1"}}) == "strategy.params.size: unknown parameter for 'null' (it takes none)");
    CHECK(error_of("null", json::array()).find("expected an object") != std::string::npos);
    bool has_null = false;
    for (const auto& n : strategy_names()) has_null |= n == "null";
    CHECK(has_null);
}

TEST_CASE("StrategyParams: typed reads with defaults, errors that name the key, unknown keys refused") {
    StrategyParams p(json{{"size", "0.002"}, {"refresh_ms", 250.5}, {"levels", 3}, {"side", "buy"}, {"post_only", true}},
                     "demo");
    CHECK(p.decimal("size", "0.001") == 200000);
    CHECK(p.decimal("max_position", "0.01") == 1000000);  // default
    CHECK(p.millis("refresh_ms", 500) == 250500000ULL);
    CHECK(p.millis("timeout_ms", 1) == 1000000ULL);
    CHECK(p.integer("levels", 1) == 3);
    CHECK(p.text("side", "sell") == "buy");
    CHECK(p.flag("post_only", false));
    CHECK_NOTHROW(p.done());

    StrategyParams q(json{{"size", "-1"}, {"levels", -2}, {"extra", 1}, {"refresh_ms", "fast"}}, "demo");
    CHECK_THROWS_WITH_AS(q.decimal("size", "1"), doctest::Contains("strategy.params.size"), std::invalid_argument);
    CHECK_THROWS_WITH_AS(q.integer("levels", 1), doctest::Contains("strategy.params.levels"), std::invalid_argument);
    CHECK_THROWS_WITH_AS(q.millis("refresh_ms", 1), doctest::Contains("strategy.params.refresh_ms"),
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(q.done(), doctest::Contains("strategy.params.extra: unknown parameter for 'demo' (levels, "
                                                     "refresh_ms, size)"),
                         std::invalid_argument);
}

TEST_CASE("the reference strategies are registered with their parameters") {
    auto q = make_strategy("passive_quoter", json{{"size", "0.002"}, {"refresh_ms", 250}, {"post_only", false}});
    const auto& qc = dynamic_cast<PassiveQuoter&>(*q).config();
    CHECK(qc.size == 200000);
    CHECK(qc.refresh_ns == 250 * kMs);
    CHECK(qc.max_position == 1000000);
    CHECK_FALSE(qc.post_only);
    auto t = make_strategy("twap_taker", json{{"side", "sell"}, {"quantity", "0.5"}, {"slices", 5}, {"duration_ms", 1000}});
    const auto& tc = dynamic_cast<TwapTaker&>(*t).config();
    CHECK(tc.side == Side::SELL);
    CHECK(tc.quantity == 50000000);
    CHECK(tc.slices == 5);
    CHECK(tc.duration_ns == 1000 * kMs);

    CHECK(error_of("passive_quoter", json{{"size", "0.02"}}) ==
          "strategy.params.max_position: must be at least size");
    CHECK(error_of("passive_quoter", json{{"refresh_ms", 0}}) == "strategy.params.refresh_ms: must be > 0");
    CHECK(error_of("passive_quoter", json{{"sise", "1"}}).find("unknown parameter for 'passive_quoter' "
                                                               "(max_position, post_only, refresh_ms, size)") !=
          std::string::npos);
    CHECK(error_of("twap_taker", json{{"side", "hold"}}) ==
          "strategy.params.side: expected \"buy\" or \"sell\", got \"hold\"");
    CHECK(error_of("twap_taker", json{{"slices", 0}}) == "strategy.params.slices: expected 1 to 1000000");
    CHECK(error_of("twap_taker", json{{"slices", 10}, {"duration_ms", 0.000001}}) ==
          "strategy.params.duration_ms: too short for the number of slices");
}

TEST_CASE("PassiveQuoter joins the best prices, follows the bid, stops at its position limit") {
    // Bid 100.00 x 5, ask 100.10 x 2. At 25 a better bid 100.05 x 3 appears; at 45 a seller takes 4 lots
    // there: the 3 lots queued ahead and the quoter's 1.
    TempFile f;
    Stream st;
    st.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    st.depth(25, {{BID, 10005, 3}});
    st.trade(45, AGGR_SELL, 10005, 4);
    st.depth(46, {{BID, 10005, 0}});
    st.write(f);

    SimulatorOptions opt;
    opt.to_ns = 100 * kMs;
    Simulator sim(f.str(), opt);
    PassiveQuoter q(PassiveQuoter::Config{kLot, 10 * kMs, kLot, true});
    sim.add_strategy(q, session("S1", 1));
    RequestLog log;
    sim.gateway().set_listener(&log);
    sim.run();

    // 0: the book isn't seen yet. 10: quotes at both bests. 30: the bid follows to 100.05 (seen at 26).
    // After the fill (learned at 46) the position is at the limit: no bid any more; the ask stays.
    CHECK(log.lines == std::vector<std::string>{"10 new buy 10000 1", "10 new sell 10010 1", "30 replace buy 10005 1"});
    const Account& a = sim.gateway().exchange_account(0);
    CHECK(a.position == kLot);
    CHECK(a.maker_fills == 1);
    CHECK(a.taker_fills == 0);
    CHECK(sim.gateway().exchange_open_orders(0) == 1);
    CHECK(q.refreshes() == 10);  // the timers at 0 .. 90 (the one at 100 is the end)
}

TEST_CASE("PassiveQuoter cancels its quotes on a gap and quotes again after the resync") {
    TempFile f;
    Stream st;
    st.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    st.r.push_back(Stream::rec(20 * static_cast<int64_t>(kMs), bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH));
    st.snapshot(55, {{BID, 10002, 5}, {ASK, 10008, 2}});
    st.write(f);

    SimulatorOptions opt;
    opt.to_ns = 70 * kMs;
    Simulator sim(f.str(), opt);
    PassiveQuoter q(PassiveQuoter::Config{kLot, 10 * kMs, 10 * kLot, true});
    sim.add_strategy(q, session("S1", 1));
    RequestLog log;
    sim.gateway().set_listener(&log);
    sim.run();

    // The gap reaches the strategy at 21: both quotes cancelled at once. Nothing while the book is invalid;
    // the snapshot is seen at 56 and the timer at 60 quotes the new prices.
    CHECK(log.lines == std::vector<std::string>{"10 new buy 10000 1", "10 new sell 10010 1", "21 cancel buy 0 0",
                                                "21 cancel sell 0 0", "60 new buy 10002 1", "60 new sell 10008 1"});
    CHECK(sim.gateway().exchange_open_orders(0) == 2);
}

TEST_CASE("TwapTaker waits for the warm-up, then sends equal MARKET slices") {
    TempFile f;
    Stream st;
    st.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 5}, {ASK, 10020, 5}});
    st.write(f);

    SimulatorOptions opt;
    opt.to_ns = 100 * kMs;
    opt.warmup_ns = 5 * kMs;
    Simulator sim(f.str(), opt);
    TwapTaker t(TwapTaker::Config{Side::BUY, 4 * kLot, 4, 40 * kMs});
    sim.add_strategy(t, session("S1", 1));
    RequestLog log;
    sim.gateway().set_listener(&log);
    sim.run();

    // On start (0) it may not trade; it polls at the slice interval (10) and starts there
    CHECK(t.started());
    CHECK(t.start_ns() == 10 * kMs);
    CHECK(log.lines == std::vector<std::string>{"10 new buy 0 1", "20 new buy 0 1", "30 new buy 0 1", "40 new buy 0 1"});
    const Account& a = sim.gateway().exchange_account(0);
    CHECK(a.position == 4 * kLot);
    CHECK(a.taker_fills == 4);
    CHECK(a.cash == -4 * 10010 * kPx / 1000);  // 4 x 0.001 at 100.10
    CHECK(t.filled() == 4 * kLot);
    CHECK(t.in_flight() == 0);
}

TEST_CASE("TwapTaker carries what a slice couldn't fill into the next one") {
    // Only 1 lot on the ask at first; more arrives at 8
    TempFile f;
    Stream st;
    st.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 1}});
    st.depth(8, {{ASK, 10020, 10}});
    st.write(f);

    SimulatorOptions opt;
    opt.from_ns = 5 * kMs;
    opt.to_ns = 50 * kMs;
    Simulator sim(f.str(), opt);
    TwapTaker t(TwapTaker::Config{Side::BUY, 4 * kLot, 2, 20 * kMs});
    sim.add_strategy(t, session("S1", 1));
    RequestLog log;
    sim.gateway().set_listener(&log);
    sim.run();

    // Slice 1 (5): 2 lots, 1 filled, 1 back unfilled (learned at 7). Slice 2 (15): the rest of the target, 3
    CHECK(log.lines == std::vector<std::string>{"5 new buy 0 2", "15 new buy 0 3"});
    CHECK(sim.gateway().exchange_account(0).position == 4 * kLot);
    CHECK(t.filled() == 4 * kLot);
}

} // TEST_SUITE("strategies")
