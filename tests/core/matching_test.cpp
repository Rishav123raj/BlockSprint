// The six original BlockSprint 1.0 unit tests, ported to doctest with unchanged assertions.
#include <doctest.h>
#include "test_helpers.hpp"

TEST_SUITE("v1") {

TEST_CASE_FIXTURE(EngineFixture, "price_time_priority") {
    const std::string symbol = "SYM-PTP";

    // Two resting bids at the same price: A (10.0) then B (5.0)
    CHECK(submit("A", symbol, OrderType::LIMIT, Side::BUY, 100.0, 10.0).empty());
    CHECK(submit("B", symbol, OrderType::LIMIT, Side::BUY, 100.0, 5.0).empty());

    nlohmann::json bbo = engine().get_bbo(symbol);
    CHECK(bbo["bid_price"].get<std::string>() == "100.000000");
    CHECK(bbo["bid_qty"].get<std::string>() == "15.000000");

    // Marketable sell C for 12.0 fills 10.0 against A, then 2.0 against B
    auto trades = submit("C", symbol, OrderType::LIMIT, Side::SELL, 100.0, 12.0);
    REQUIRE(trades.size() == 2U);

    CHECK(trades[0].maker_order_id == "A");
    CHECK(trades[0].taker_order_id == "C");
    CHECK(trades[0].quantity == fx(10.0));
    CHECK(trades[0].price == fx(100.0));

    CHECK(trades[1].maker_order_id == "B");
    CHECK(trades[1].taker_order_id == "C");
    CHECK(trades[1].quantity == fx(2.0));
    CHECK(trades[1].price == fx(100.0));

    // B has 3.0 remaining
    nlohmann::json bbo_after = engine().get_bbo(symbol);
    CHECK(bbo_after["bid_price"].get<std::string>() == "100.000000");
    CHECK(bbo_after["bid_qty"].get<std::string>() == "3.000000");
}

TEST_CASE_FIXTURE(EngineFixture, "internal_trade_through_protection") {
    const std::string symbol = "SYM-TTP";

    submit("A", symbol, OrderType::LIMIT, Side::SELL, 101.0, 10.0); // best ask
    submit("B", symbol, OrderType::LIMIT, Side::SELL, 102.0, 10.0); // worse ask

    // Buy 15.0 at 103.0 must fill at 101.0 first, then move to 102.0
    auto trades = submit("C", symbol, OrderType::LIMIT, Side::BUY, 103.0, 15.0);
    REQUIRE(trades.size() == 2U);

    CHECK(trades[0].maker_order_id == "A");
    CHECK(trades[0].price == fx(101.0));
    CHECK(trades[0].quantity == fx(10.0));

    CHECK(trades[1].maker_order_id == "B");
    CHECK(trades[1].price == fx(102.0));
    CHECK(trades[1].quantity == fx(5.0));

    nlohmann::json bbo = engine().get_bbo(symbol);
    CHECK(bbo["ask_price"].get<std::string>() == "102.000000");
    CHECK(bbo["ask_qty"].get<std::string>() == "5.000000");
}

TEST_CASE_FIXTURE(EngineFixture, "ioc_order_handling") {
    const std::string symbol = "SYM-IOC";

    submit("A", symbol, OrderType::LIMIT, Side::SELL, 100.0, 10.0);

    // Buy IOC 15.0 fills 10.0; the remaining 5.0 is cancelled, not rested
    auto trades = submit("IOC_BUY", symbol, OrderType::IOC, Side::BUY, 100.0, 15.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].quantity == fx(10.0));

    nlohmann::json bbo = engine().get_bbo(symbol);
    CHECK(bbo["bid_price"].get<std::string>() == "0.0");
    CHECK(bbo["ask_price"].get<std::string>() == "0.0");
}

TEST_CASE_FIXTURE(EngineFixture, "fok_order_handling") {
    const std::string symbol = "SYM-FOK";

    submit("A", symbol, OrderType::LIMIT, Side::SELL, 100.0, 10.0);

    // FOK for 15.0 cannot be fully filled: killed with no trades
    CHECK(submit("FOK_FAIL", symbol, OrderType::FOK, Side::BUY, 100.0, 15.0).empty());

    nlohmann::json bbo1 = engine().get_bbo(symbol);
    CHECK(bbo1["ask_price"].get<std::string>() == "100.000000");
    CHECK(bbo1["ask_qty"].get<std::string>() == "10.000000");

    // FOK for exactly 10.0 fills completely
    auto trades = submit("FOK_SUCCESS", symbol, OrderType::FOK, Side::BUY, 100.0, 10.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].quantity == fx(10.0));

    nlohmann::json bbo2 = engine().get_bbo(symbol);
    CHECK(bbo2["ask_price"].get<std::string>() == "0.0");
}

TEST_CASE_FIXTURE(EngineFixture, "order_cancellation") {
    const std::string symbol = "SYM-CANCEL";

    submit("A", symbol, OrderType::LIMIT, Side::BUY, 100.0, 10.0);
    CHECK(engine().get_bbo(symbol)["bid_price"].get<std::string>() == "100.000000");

    CHECK(engine().cancel_order(symbol, "A"));
    CHECK(engine().get_bbo(symbol)["bid_price"].get<std::string>() == "0.0");

    // Cancelling again fails
    CHECK_FALSE(engine().cancel_order(symbol, "A"));
}

TEST_CASE_FIXTURE(EngineFixture, "order_modification") {
    const std::string symbol = "SYM-MODIFY";

    submit("A", symbol, OrderType::LIMIT, Side::BUY, 100.0, 10.0);
    submit("B", symbol, OrderType::LIMIT, Side::BUY, 100.0, 5.0);

    // Reducing A's quantity keeps its time priority
    std::vector<Trade> trades_mod;
    CHECK(engine().modify_order(symbol, "A", fx(4.0), fx(100.0), trades_mod));
    CHECK(trades_mod.empty());

    // Sell 6.0 fills 4.0 against A (still first), then 2.0 against B
    auto trades = submit("C", symbol, OrderType::LIMIT, Side::SELL, 100.0, 6.0);
    REQUIRE(trades.size() == 2U);
    CHECK(trades[0].maker_order_id == "A");
    CHECK(trades[0].quantity == fx(4.0));
    CHECK(trades[1].maker_order_id == "B");
    CHECK(trades[1].quantity == fx(2.0));
}

} // TEST_SUITE("v1")
