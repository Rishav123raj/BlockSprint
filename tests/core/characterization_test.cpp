// Characterization tests: pin the exact matching behavior of BlockSprint 1.0 so that
// the Phase 1 refactors (unified matching loop, integer ticks, ...) cannot change it by accident.
//
// Cases in the "v1-quirk" suite document known 1.0 defects (see the integration guide, section 3).
// They are expected to be rewritten when the task that fixes the defect lands (e.g. P1.9 for D7/D9).
#include <doctest.h>
#include "workload.hpp"
#include <utility>
#include <vector>

namespace {

using Levels = std::vector<std::pair<double, double>>;

struct Depth {
    Levels bids;
    Levels asks;
};

Depth depth(MatchingEngine& eng, const std::string& symbol, int levels = 1000) {
    nlohmann::json j = eng.get_l2_depth(symbol, levels);
    Depth d;
    for (const auto& lvl : j["bids"]) {
        d.bids.emplace_back(std::stod(lvl[0].get<std::string>()), std::stod(lvl[1].get<std::string>()));
    }
    for (const auto& lvl : j["asks"]) {
        d.asks.emplace_back(std::stod(lvl[0].get<std::string>()), std::stod(lvl[1].get<std::string>()));
    }
    return d;
}

} // namespace

TEST_SUITE("characterization") {

TEST_CASE_FIXTURE(EngineFixture, "limit buy sweeps levels in price order and rests the remainder at its limit") {
    const std::string s = "CH-SWEEP-BUY";
    submit("A", s, OrderType::LIMIT, Side::SELL, 101.0, 2.0);
    submit("B", s, OrderType::LIMIT, Side::SELL, 102.0, 3.0);
    submit("C", s, OrderType::LIMIT, Side::SELL, 103.0, 5.0);

    auto trades = submit("T", s, OrderType::LIMIT, Side::BUY, 102.5, 7.0);
    REQUIRE(trades.size() == 2U);
    CHECK(trades[0].maker_order_id == "A");
    CHECK(trades[0].price == fx(101.0));
    CHECK(trades[0].quantity == fx(2.0));
    CHECK(trades[1].maker_order_id == "B");
    CHECK(trades[1].price == fx(102.0));
    CHECK(trades[1].quantity == fx(3.0));

    Depth d = depth(engine(), s);
    CHECK(d.bids == Levels{{102.5, 2.0}});
    CHECK(d.asks == Levels{{103.0, 5.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "limit sell sweeps bids from highest to lowest") {
    const std::string s = "CH-SWEEP-SELL";
    submit("A", s, OrderType::LIMIT, Side::BUY, 99.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::BUY, 98.0, 1.0);
    submit("C", s, OrderType::LIMIT, Side::BUY, 97.0, 1.0);

    auto trades = submit("T", s, OrderType::LIMIT, Side::SELL, 98.0, 3.0);
    REQUIRE(trades.size() == 2U);
    CHECK(trades[0].maker_order_id == "A");
    CHECK(trades[0].price == fx(99.0));
    CHECK(trades[1].maker_order_id == "B");
    CHECK(trades[1].price == fx(98.0));

    Depth d = depth(engine(), s);
    CHECK(d.bids == Levels{{97.0, 1.0}});
    CHECK(d.asks == Levels{{98.0, 1.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "market order sweeps every level and never rests") {
    const std::string s = "CH-MKT";
    submit("A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::SELL, 102.0, 1.0);

    auto trades = submit("T", s, OrderType::MARKET, Side::BUY, 0.0, 5.0);
    REQUIRE(trades.size() == 2U);
    CHECK(trades[0].price == fx(101.0));
    CHECK(trades[1].price == fx(102.0));

    Depth d = depth(engine(), s);
    CHECK(d.bids.empty());
    CHECK(d.asks.empty());
}

TEST_CASE_FIXTURE(EngineFixture, "market order on an empty book produces no trades and does not rest") {
    const std::string s = "CH-MKT-EMPTY";
    CHECK(submit("T", s, OrderType::MARKET, Side::SELL, 0.0, 1.0).empty());
    Depth d = depth(engine(), s);
    CHECK(d.bids.empty());
    CHECK(d.asks.empty());
}

TEST_CASE_FIXTURE(EngineFixture, "IOC stops at its limit price and discards the remainder") {
    const std::string s = "CH-IOC-LIMIT";
    submit("A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::SELL, 102.0, 1.0);

    auto trades = submit("T", s, OrderType::IOC, Side::BUY, 101.0, 5.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].price == fx(101.0));

    Depth d = depth(engine(), s);
    CHECK(d.bids.empty());
    CHECK(d.asks == Levels{{102.0, 1.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "FOK counts only liquidity within its limit price") {
    const std::string s = "CH-FOK-LEVELS";
    submit("A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::SELL, 102.0, 1.0);

    // Enough total liquidity, but not at or below 101.0: killed
    CHECK(submit("F1", s, OrderType::FOK, Side::BUY, 101.0, 2.0).empty());
    CHECK(depth(engine(), s).asks == Levels{{101.0, 1.0}, {102.0, 1.0}});

    // With a 102.0 limit it fills across both levels
    auto trades = submit("F2", s, OrderType::FOK, Side::BUY, 102.0, 2.0);
    REQUIRE(trades.size() == 2U);
    CHECK(depth(engine(), s).asks.empty());
}

TEST_CASE_FIXTURE(EngineFixture, "trade carries maker price, taker side and increasing trade ids") {
    const std::string s = "CH-TRADE-FIELDS";
    submit("M1", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("M2", s, OrderType::LIMIT, Side::BUY, 99.5, 1.0);

    auto trades = submit("T", s, OrderType::LIMIT, Side::SELL, 99.0, 2.0);
    REQUIRE(trades.size() == 2U);
    CHECK(trades[0].price == fx(100.0)); // maker's price, not the taker's 99.0
    CHECK(trades[1].price == fx(99.5));
    CHECK(trades[0].aggressor_side == Side::SELL);
    CHECK(trades[0].symbol == s);
    CHECK(trades[0].maker_order_id == "M1");
    CHECK(trades[0].taker_order_id == "T");
    CHECK(std::stoull(trades[1].trade_id) > std::stoull(trades[0].trade_id));
}

TEST_CASE_FIXTURE(EngineFixture, "FIFO within a level including partial maker fills") {
    const std::string s = "CH-FIFO";
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::BUY, 100.0, 2.0);
    submit("C", s, OrderType::LIMIT, Side::BUY, 100.0, 3.0);

    auto t1 = submit("S1", s, OrderType::LIMIT, Side::SELL, 100.0, 2.5);
    REQUIRE(t1.size() == 2U);
    CHECK(t1[0].maker_order_id == "A");
    CHECK(t1[0].quantity == fx(1.0));
    CHECK(t1[1].maker_order_id == "B");
    CHECK(t1[1].quantity == fx(1.5));

    auto t2 = submit("S2", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    REQUIRE(t2.size() == 2U);
    CHECK(t2[0].maker_order_id == "B");
    CHECK(t2[0].quantity == fx(0.5));
    CHECK(t2[1].maker_order_id == "C");
    CHECK(t2[1].quantity == fx(0.5));

    CHECK(depth(engine(), s).bids == Levels{{100.0, 2.5}});
}

TEST_CASE_FIXTURE(EngineFixture, "modify: price change loses time priority") {
    const std::string s = "CH-MOD-PRICE";
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    std::vector<Trade> tm;
    CHECK(engine().modify_order(s, "A", fx(1.0), fx(99.0), tm));
    CHECK(engine().modify_order(s, "A", fx(1.0), fx(100.0), tm));

    auto trades = submit("S", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].maker_order_id == "B");
}

TEST_CASE_FIXTURE(EngineFixture, "modify: quantity increase loses time priority") {
    const std::string s = "CH-MOD-UP";
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    std::vector<Trade> tm;
    CHECK(engine().modify_order(s, "A", fx(2.0), fx(100.0), tm));
    CHECK(depth(engine(), s).bids == Levels{{100.0, 3.0}});

    auto trades = submit("S", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].maker_order_id == "B");
}

TEST_CASE_FIXTURE(EngineFixture, "modify: quantity to zero cancels the order") {
    const std::string s = "CH-MOD-ZERO";
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    std::vector<Trade> tm;
    CHECK(engine().modify_order(s, "A", fx(0.0), fx(100.0), tm));
    CHECK(depth(engine(), s).bids.empty());
    CHECK_FALSE(engine().cancel_order(s, "A"));
}

TEST_CASE_FIXTURE(EngineFixture, "modify: unknown order id returns false") {
    std::vector<Trade> tm;
    CHECK_FALSE(engine().modify_order("CH-MOD-UNKNOWN", "nope", fx(1.0), fx(100.0), tm));
}

TEST_CASE_FIXTURE(EngineFixture, "modify: re-pricing through the spread executes") {
    const std::string s = "CH-MOD-CROSS";
    submit("X", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    std::vector<Trade> tm;
    CHECK(engine().modify_order(s, "A", fx(1.0), fx(101.0), tm));
    REQUIRE(tm.size() == 1U);
    CHECK(tm[0].maker_order_id == "X");
    CHECK(tm[0].taker_order_id == "A");
    CHECK(tm[0].price == fx(101.0));

    Depth d = depth(engine(), s);
    CHECK(d.bids.empty());
    CHECK(d.asks.empty());
}

TEST_CASE_FIXTURE(EngineFixture, "cancel of a partially filled order removes only its remaining quantity") {
    const std::string s = "CH-CANCEL-PARTIAL";
    submit("A", s, OrderType::LIMIT, Side::SELL, 101.0, 5.0);
    submit("B", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("T", s, OrderType::LIMIT, Side::BUY, 101.0, 2.0);
    CHECK(depth(engine(), s).asks == Levels{{101.0, 4.0}});

    CHECK(engine().cancel_order(s, "A"));
    CHECK(depth(engine(), s).asks == Levels{{101.0, 1.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "STP: BINANCE_ orders skip each other and match other makers behind them") {
    const std::string s = "CH-STP";
    submit("BINANCE_A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("U1", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);

    auto trades = submit("BINANCE_B", s, OrderType::LIMIT, Side::BUY, 101.0, 1.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].maker_order_id == "U1");

    Depth d = depth(engine(), s);
    CHECK(d.bids.empty());
    CHECK(d.asks == Levels{{101.0, 1.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "L2 depth aggregates per level, is best-first and respects the depth limit") {
    const std::string s = "CH-DEPTH";
    submit("B1", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("B2", s, OrderType::LIMIT, Side::BUY, 100.0, 2.0);
    submit("B3", s, OrderType::LIMIT, Side::BUY, 99.0, 1.0);
    submit("B4", s, OrderType::LIMIT, Side::BUY, 98.0, 1.0);
    submit("A1", s, OrderType::LIMIT, Side::SELL, 102.0, 1.0);
    submit("A2", s, OrderType::LIMIT, Side::SELL, 101.0, 4.0);

    Depth d = depth(engine(), s, 2);
    CHECK(d.bids == Levels{{100.0, 3.0}, {99.0, 1.0}});
    CHECK(d.asks == Levels{{101.0, 4.0}, {102.0, 1.0}});

    nlohmann::json j = engine().get_l2_depth(s, 1);
    CHECK(j["bids"][0][0].get<std::string>() == "100.000000");
    CHECK(j["symbol"].get<std::string>() == s);
}

TEST_CASE_FIXTURE(EngineFixture, "callbacks: when trade, BBO and depth events fire") {
    const std::string s = "CH-CALLBACKS";
    int trade_events = 0;
    int bbo_events = 0;
    int depth_events = 0;
    engine().register_trade_callback([&](const std::string&, const Trade&) { ++trade_events; });
    engine().register_bbo_callback([&](const std::string&, const nlohmann::json&) { ++bbo_events; });
    engine().register_depth_callback([&](const std::string&, const nlohmann::json&) { ++depth_events; });

    // New best bid: BBO changed, and every LIMIT submit publishes depth
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    CHECK(trade_events == 0);
    CHECK(bbo_events == 1);
    CHECK(depth_events == 1);

    // Bid behind the best: BBO unchanged, depth still published
    submit("B", s, OrderType::LIMIT, Side::BUY, 99.0, 1.0);
    CHECK(bbo_events == 1);
    CHECK(depth_events == 2);

    // Market sell trades with A
    submit("M", s, OrderType::MARKET, Side::SELL, 0.0, 1.0);
    CHECK(trade_events == 1);
    CHECK(bbo_events == 2);
    CHECK(depth_events == 3);

    // Successful cancel always publishes BBO and depth
    CHECK(engine().cancel_order(s, "B"));
    CHECK(bbo_events == 3);
    CHECK(depth_events == 4);

    // Market order on an empty side: nothing changes, nothing published
    submit("M2", s, OrderType::MARKET, Side::SELL, 0.0, 1.0);
    CHECK(trade_events == 1);
    CHECK(bbo_events == 3);
    CHECK(depth_events == 4);

    engine().register_trade_callback({});
    engine().register_bbo_callback({});
    engine().register_depth_callback({});
}

TEST_CASE_FIXTURE(EngineFixture, "golden random workload reproduces the v1.0.0 trade stream exactly") {
    WorkloadStats stats = run_random_workload(engine(), "CH-GOLDEN", 20261101ULL, 5000);
    MESSAGE("golden hash = " << stats.hash << ", trades = " << stats.trades << ", cancels_ok = " << stats.cancels_ok
                             << ", modifies_ok = " << stats.modifies_ok);

    // Make sure the workload actually exercises matching, cancels and modifies
    CHECK(stats.trades > 1000);
    CHECK(stats.cancels_ok > 50);
    CHECK(stats.modifies_ok > 50);

    // Pinned from the original v1.0.0 sources (tag f1fc45f); identical on MSVC 2022 and GCC 15
    CHECK(stats.hash == 11732550826495055626ULL);
}

TEST_CASE_FIXTURE(EngineFixture, "golden random workload is deterministic within a process") {
    WorkloadStats first = run_random_workload(engine(), "CH-DET-1", 7ULL, 2000);
    WorkloadStats second = run_random_workload(engine(), "CH-DET-2", 7ULL, 2000);
    CHECK(first.hash == second.hash);
    CHECK(first.trades == second.trades);
}

} // TEST_SUITE("characterization")

TEST_SUITE("v1-quirk") {

// D7: when STP skips every maker at a crossing level, the order rests through the opposite side.
// To be replaced in P1.9 ("never rest crossed").
TEST_CASE_FIXTURE(EngineFixture, "D7: STP can leave the book crossed") {
    const std::string s = "QK-D7";
    submit("BINANCE_A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);

    CHECK(submit("BINANCE_B", s, OrderType::LIMIT, Side::BUY, 102.0, 1.0).empty());
    Depth d = depth(engine(), s);
    CHECK(d.bids == Levels{{102.0, 1.0}});
    CHECK(d.asks == Levels{{101.0, 1.0}});
}

// D21 (found by the golden workload during P1.4): the FOK dry run counts liquidity on deeper levels past
// self-trade makers, but the sweep stops at the level holding them. The FOK then partially fills and the
// remainder rests as a limit order, crossing the book. To be replaced in P1.9 (FOK never rests).
TEST_CASE_FIXTURE(EngineFixture, "D21: FOK blocked by self-trade makers partially fills and rests") {
    const std::string s = "QK-D21";
    submit("U0", s, OrderType::LIMIT, Side::SELL, 100.0, 0.5);
    submit("BINANCE_A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("U1", s, OrderType::LIMIT, Side::SELL, 102.0, 1.0);

    auto trades = submit("BINANCE_B", s, OrderType::FOK, Side::BUY, 102.0, 1.5);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].maker_order_id == "U0");
    CHECK(trades[0].quantity == fx(0.5));

    Depth d = depth(engine(), s);
    CHECK(d.bids == Levels{{102.0, 1.0}});
    CHECK(d.asks == Levels{{101.0, 1.0}, {102.0, 1.0}});
}

// D9: a duplicate order id silently cancels the resting order and re-queues the new one.
// To be replaced in P1.9 (duplicate id => REJECT, explicit replace).
TEST_CASE_FIXTURE(EngineFixture, "D9: duplicate order id silently replaces the resting order") {
    const std::string s = "QK-D9";
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    submit("A", s, OrderType::LIMIT, Side::BUY, 99.0, 2.0);
    CHECK(depth(engine(), s).bids == Levels{{100.0, 1.0}, {99.0, 2.0}});
}

} // TEST_SUITE("v1-quirk")
