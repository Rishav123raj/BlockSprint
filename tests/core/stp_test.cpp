// Self-trade prevention by order owner (P1.9), and the 1.0 defects it fixes: D7, D9, D21.
#include <doctest.h>
#include "test_helpers.hpp"
#include <utility>
#include <vector>

namespace {

using Levels = std::vector<std::pair<double, double>>;

Levels side_levels(const nlohmann::json& side) {
    Levels levels;
    for (const auto& lvl : side) {
        levels.emplace_back(std::stod(lvl[0].get<std::string>()), std::stod(lvl[1].get<std::string>()));
    }
    return levels;
}

Levels asks(MatchingEngine& eng, const std::string& s) { return side_levels(eng.get_l2_depth(s, 100)["asks"]); }
Levels bids(MatchingEngine& eng, const std::string& s) { return side_levels(eng.get_l2_depth(s, 100)["bids"]); }

} // namespace

TEST_SUITE("stp") {

TEST_CASE_FIXTURE(EngineFixture, "each policy against the same book") {
    // Asks: own order (owner A) and another participant's order at 100, an anonymous order at 101.
    // Incoming: owner A buys 2.0 up to 101.
    const std::string s = "STP-MATRIX";
    auto setup = [&] {
        submit_as("A", StpPolicy::CANCEL_MAKER, "OWN", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
        submit_as("B", StpPolicy::CANCEL_MAKER, "OTHER", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
        submit("ANON", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    };

    SUBCASE("NONE: self-trades are allowed") {
        setup();
        auto t = submit_as("A", StpPolicy::NONE, "T", s, OrderType::LIMIT, Side::BUY, 101.0, 2.0);
        REQUIRE(t.size() == 2U);
        CHECK(t[0].maker_order_id == "OWN");
        CHECK(t[1].maker_order_id == "OTHER");
        CHECK(asks(engine(), s) == Levels{{101.0, 1.0}});
    }
    SUBCASE("SKIP: own order is passed over, matching continues behind it and at the next level") {
        setup();
        auto t = submit_as("A", StpPolicy::SKIP, "T", s, OrderType::LIMIT, Side::BUY, 101.0, 2.0);
        REQUIRE(t.size() == 2U);
        CHECK(t[0].maker_order_id == "OTHER");
        CHECK(t[1].maker_order_id == "ANON");
        CHECK(t[1].price == fx(101.0));
        CHECK(asks(engine(), s) == Levels{{100.0, 1.0}}); // the skipped own order still rests
        CHECK(bids(engine(), s).empty());
    }
    SUBCASE("CANCEL_MAKER: own order is cancelled, matching continues") {
        setup();
        auto t = submit_as("A", StpPolicy::CANCEL_MAKER, "T", s, OrderType::LIMIT, Side::BUY, 101.0, 2.0);
        REQUIRE(t.size() == 2U);
        CHECK(t[0].maker_order_id == "OTHER");
        CHECK(t[1].maker_order_id == "ANON");
        CHECK(asks(engine(), s).empty());
        CHECK_FALSE(engine().cancel_order(s, "OWN")); // already gone
    }
    SUBCASE("CANCEL_TAKER: matching stops at the own order and the incoming order is cancelled") {
        setup();
        CHECK(submit_as("A", StpPolicy::CANCEL_TAKER, "T", s, OrderType::LIMIT, Side::BUY, 101.0, 2.0).empty());
        CHECK(asks(engine(), s) == Levels{{100.0, 2.0}, {101.0, 1.0}});
        CHECK(bids(engine(), s).empty()); // the remainder did not rest
    }
    SUBCASE("CANCEL_BOTH: own order and incoming order are both cancelled") {
        setup();
        CHECK(submit_as("A", StpPolicy::CANCEL_BOTH, "T", s, OrderType::LIMIT, Side::BUY, 101.0, 2.0).empty());
        CHECK(asks(engine(), s) == Levels{{100.0, 1.0}, {101.0, 1.0}});
        CHECK(bids(engine(), s).empty());
        CHECK_FALSE(engine().cancel_order(s, "OWN"));
    }
    CHECK(engine().check_invariants(s).empty());
}

TEST_CASE_FIXTURE(EngineFixture, "ownership rules") {
    const std::string s = "STP-OWNERS";

    SUBCASE("orders without an owner never count as self-trades") {
        submit("X", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
        auto t = submit("Y", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
        CHECK(t.size() == 1U);
    }
    SUBCASE("different owners trade normally") {
        submit_as("A", StpPolicy::CANCEL_TAKER, "X", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
        auto t = submit_as("B", StpPolicy::CANCEL_TAKER, "Y", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
        CHECK(t.size() == 1U);
    }
    SUBCASE("the default policy is CANCEL_MAKER, as on Binance") {
        CHECK(Order{}.stp_policy == StpPolicy::CANCEL_MAKER);
        auto own = make_order("X", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
        own->owner_id = "A";
        engine().submit_order(own);
        auto incoming = make_order("Y", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
        incoming->owner_id = "A";
        CHECK(engine().submit_order(incoming).empty());
        CHECK(asks(engine(), s).empty());
        CHECK(bids(engine(), s) == Levels{{100.0, 1.0}}); // nothing left to cross, so it rests
    }
    SUBCASE("the incoming order's policy decides, not the resting order's") {
        submit_as("A", StpPolicy::NONE, "X", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
        CHECK(submit_as("A", StpPolicy::CANCEL_TAKER, "Y", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0).empty());
        CHECK(asks(engine(), s) == Levels{{100.0, 1.0}});
    }
}

TEST_CASE("stp policy names round-trip") {
    for (StpPolicy p : {StpPolicy::NONE, StpPolicy::SKIP, StpPolicy::CANCEL_TAKER, StpPolicy::CANCEL_MAKER,
                        StpPolicy::CANCEL_BOTH}) {
        CHECK(stringToStpPolicy(stpPolicyToString(p)) == p);
    }
    CHECK_THROWS_AS(stringToStpPolicy("expire_maker"), std::invalid_argument);
}

TEST_CASE_FIXTURE(EngineFixture, "D7 fixed: a remainder that would cross after skipping is cancelled, not rested") {
    const std::string s = "STP-D7";
    submit_as("MARKET", StpPolicy::SKIP, "M1", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);

    // 1.0 rested this bid at 102 above the 101 ask
    CHECK(submit_as("MARKET", StpPolicy::SKIP, "M2", s, OrderType::LIMIT, Side::BUY, 102.0, 1.0).empty());
    CHECK(bids(engine(), s).empty());
    CHECK(asks(engine(), s) == Levels{{101.0, 1.0}});
    CHECK_FALSE(engine().is_crossed(s));

    // A partial fill elsewhere does not change that: the remainder still may not rest crossed
    submit("U", s, OrderType::LIMIT, Side::SELL, 101.5, 0.5);
    auto t = submit_as("MARKET", StpPolicy::SKIP, "M3", s, OrderType::LIMIT, Side::BUY, 102.0, 2.0);
    REQUIRE(t.size() == 1U);
    CHECK(t[0].maker_order_id == "U");
    CHECK(bids(engine(), s).empty());
    CHECK(engine().check_invariants(s).empty());
}

TEST_CASE_FIXTURE(EngineFixture, "D7 fixed: a non-crossing remainder still rests") {
    const std::string s = "STP-D7-REST";
    submit("U", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    auto t = submit_as("MARKET", StpPolicy::SKIP, "M", s, OrderType::LIMIT, Side::BUY, 101.0, 3.0);
    REQUIRE(t.size() == 1U);
    CHECK(bids(engine(), s) == Levels{{101.0, 2.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "D21 fixed: the FOK dry run and the sweep follow the same self-trade rules") {
    const std::string s = "STP-D21";
    auto setup = [&] {
        submit("U0", s, OrderType::LIMIT, Side::SELL, 100.0, 0.5);
        submit_as("MARKET", StpPolicy::SKIP, "M1", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
        submit("U1", s, OrderType::LIMIT, Side::SELL, 102.0, 1.0);
    };

    SUBCASE("SKIP: liquidity behind the skipped order counts, and the FOK fills completely") {
        setup();
        // 1.0 filled 0.5 and rested the remaining 1.0 as a bid at 102, crossing the book
        auto t = submit_as("MARKET", StpPolicy::SKIP, "F", s, OrderType::FOK, Side::BUY, 102.0, 1.5);
        REQUIRE(t.size() == 2U);
        CHECK(t[0].maker_order_id == "U0");
        CHECK(t[1].maker_order_id == "U1");
        CHECK(t[0].quantity + t[1].quantity == fx(1.5));
        CHECK(bids(engine(), s).empty());
        CHECK(asks(engine(), s) == Levels{{101.0, 1.0}});
    }
    SUBCASE("CANCEL_TAKER: the own order ends the dry run, so the FOK is killed untouched") {
        setup();
        CHECK(submit_as("MARKET", StpPolicy::CANCEL_TAKER, "F", s, OrderType::FOK, Side::BUY, 102.0, 1.5).empty());
        CHECK(asks(engine(), s) == Levels{{100.0, 0.5}, {101.0, 1.0}, {102.0, 1.0}});
        CHECK(bids(engine(), s).empty());
    }
    SUBCASE("CANCEL_MAKER: the own order is cancelled during execution and the FOK fills") {
        setup();
        auto t = submit_as("MARKET", StpPolicy::CANCEL_MAKER, "F", s, OrderType::FOK, Side::BUY, 102.0, 1.5);
        REQUIRE(t.size() == 2U);
        CHECK(asks(engine(), s).empty());
    }
    SUBCASE("a killed FOK leaves own orders alone, even with CANCEL_MAKER") {
        setup();
        CHECK(submit_as("MARKET", StpPolicy::CANCEL_MAKER, "F", s, OrderType::FOK, Side::BUY, 102.0, 5.0).empty());
        CHECK(asks(engine(), s) == Levels{{100.0, 0.5}, {101.0, 1.0}, {102.0, 1.0}});
    }
    CHECK(engine().check_invariants(s).empty());
}

TEST_CASE_FIXTURE(EngineFixture, "D9 fixed: a duplicate order ID is rejected; replace is explicit") {
    const std::string s = "STP-D9";
    submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    // 1.0 silently cancelled A and re-queued the new order; now it is rejected with a report
    std::vector<ExecReport> reports;
    CHECK(engine().submit_order(make_order("A", s, OrderType::LIMIT, Side::BUY, 99.0, 2.0), &reports).empty());
    REQUIRE(reports.size() == 1U);
    CHECK(reports[0].type == ExecType::REJECTED);
    CHECK(reports[0].reason == ExecReason::DUPLICATE_ORDER_ID);
    CHECK(bids(engine(), s) == Levels{{100.0, 2.0}});

    // Replace moves A to its new price and quantity
    auto replaced = engine().replace_order(make_order("A", s, OrderType::LIMIT, Side::BUY, 99.0, 2.0));
    CHECK(replaced.empty());
    CHECK(bids(engine(), s) == Levels{{100.0, 1.0}, {99.0, 2.0}});

    // Replacing an unknown ID simply submits it
    engine().replace_order(make_order("C", s, OrderType::LIMIT, Side::BUY, 98.0, 1.0));
    CHECK(bids(engine(), s) == Levels{{100.0, 1.0}, {99.0, 2.0}, {98.0, 1.0}});

    // An ID can be reused once its order is no longer resting
    submit("S", s, OrderType::MARKET, Side::SELL, 0.0, 1.0); // fills B
    CHECK(submit("B", s, OrderType::LIMIT, Side::BUY, 97.0, 1.0).empty());
    CHECK(engine().check_invariants(s).empty());
}

TEST_CASE_FIXTURE(EngineFixture, "replace re-queues at the back of its level") {
    const std::string s = "STP-REPLACE-QUEUE";
    submit("A", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    engine().replace_order(make_order("A", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0));

    auto t = submit("T", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    REQUIRE(t.size() == 1U);
    CHECK(t[0].maker_order_id == "B");
}

TEST_CASE_FIXTURE(EngineFixture, "modify keeps the order's owner and STP policy") {
    const std::string s = "STP-MODIFY";
    submit_as("MARKET", StpPolicy::SKIP, "X", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit_as("MARKET", StpPolicy::SKIP, "Y", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    // Re-pricing Y through X: as a MARKET/SKIP order it must not trade with X (an anonymous copy would)
    std::vector<Trade> trades;
    CHECK(engine().modify_order(s, "Y", fx(1.0), fx(101.0), trades));
    CHECK(trades.empty());
    CHECK(asks(engine(), s) == Levels{{101.0, 1.0}});
    CHECK(bids(engine(), s).empty()); // would cross, so cancelled
    CHECK(engine().check_invariants(s).empty());
}

} // TEST_SUITE("stp")
