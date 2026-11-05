// Order validation and symbol trading rules (P1.12): fixes D6, D12, D18.
#include <doctest.h>
#include "test_helpers.hpp"
#include <limits>

namespace {

// Binance's BTCUSDT spot filters: PRICE_FILTER tickSize 0.01, LOT_SIZE stepSize/minQty 0.00001,
// maxQty 9000, NOTIONAL minNotional 5
SymbolSpec btc_spec(const std::string& symbol = "BTC-USDT") {
    SymbolSpec spec;
    spec.symbol = symbol;
    spec.tick_size = fx(0.01);
    spec.lot_size = 1000;   // 0.00001
    spec.min_qty = 1000;    // 0.00001
    spec.max_qty = fx(9000.0);
    spec.min_notional = fx(5.0);
    return spec;
}

std::optional<ExecReason> reason(const Order& order, const SymbolSpec* spec = nullptr) {
    auto error = validate_order(order, spec);
    return error ? std::optional<ExecReason>(error->reason) : std::nullopt;
}

ExecReport last_report(MatchingEngine& eng, std::shared_ptr<Order> order) {
    std::vector<ExecReport> reports;
    eng.submit_order(order, &reports);
    REQUIRE_FALSE(reports.empty());
    return reports.back();
}

} // namespace

TEST_SUITE("validation") {

TEST_CASE("basic rules apply to every symbol") {
    CHECK(reason(*make_order("A", "X", OrderType::LIMIT, Side::BUY, 100.0, 0.0)) == ExecReason::INVALID_QUANTITY);
    auto negative = make_order("A", "X", OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    negative->quantity = -5;
    CHECK(reason(*negative) == ExecReason::INVALID_QUANTITY);

    for (OrderType type : {OrderType::LIMIT, OrderType::IOC, OrderType::FOK}) {
        CAPTURE(orderTypeToString(type));
        CHECK(reason(*make_order("A", "X", type, Side::SELL, 0.0, 1.0)) == ExecReason::INVALID_PRICE);
    }
    CHECK_FALSE(reason(*make_order("A", "X", OrderType::MARKET, Side::SELL, 0.0, 1.0)).has_value());
    CHECK_FALSE(reason(*make_order("A", "X", OrderType::LIMIT, Side::SELL, 0.00000001, 0.00000001)).has_value());
}

TEST_CASE("symbol spec: tick size, lot size, quantity range, min notional") {
    SymbolSpec spec = btc_spec();
    auto check = [&](double price, double qty, OrderType type = OrderType::LIMIT) {
        return reason(*make_order("A", "BTC-USDT", type, Side::BUY, price, qty), &spec);
    };

    CHECK_FALSE(check(77000.01, 0.00123).has_value());
    CHECK(check(77000.005, 0.001) == ExecReason::PRICE_TICK);
    CHECK(check(77000.01, 0.000015) == ExecReason::QUANTITY_LOT);
    CHECK(check(77000.01, 9000.00001) == ExecReason::QUANTITY_RANGE);
    CHECK_FALSE(check(77000.01, 9000.0).has_value());  // max is inclusive

    // Min notional 5: 100 x 0.04 = 4 is too small, 100 x 0.05 = 5 is exactly enough
    CHECK(check(100.0, 0.04) == ExecReason::MIN_NOTIONAL);
    CHECK_FALSE(check(100.0, 0.05).has_value());

    // MARKET orders skip tick and notional (no price yet) but keep the quantity rules
    CHECK_FALSE(check(0.0, 0.00001, OrderType::MARKET).has_value());
    CHECK(check(0.0, 0.000015, OrderType::MARKET) == ExecReason::QUANTITY_LOT);

    // Messages explain the rule
    auto error = validate_order(*make_order("A", "BTC-USDT", OrderType::LIMIT, Side::BUY, 77000.005, 1.0), &spec);
    REQUIRE(error);
    CHECK(error->text.find("not a multiple of the tick size 0.01") != std::string::npos);
}

TEST_CASE("market-data liquidity is exempt from symbol rules but not from the basic checks") {
    SymbolSpec spec = btc_spec();
    auto dust = make_order("L", "BTC-USDT", OrderType::LIMIT, Side::SELL, 77000.0, 0.00001);  // 0.77 USDT
    CHECK(reason(*dust, &spec) == ExecReason::MIN_NOTIONAL);
    dust->market_data_liquidity = true;
    CHECK_FALSE(reason(*dust, &spec).has_value());

    auto free_price = make_order("L", "BTC-USDT", OrderType::LIMIT, Side::SELL, 0.0, 1.0);
    free_price->market_data_liquidity = true;
    CHECK(reason(*free_price, &spec) == ExecReason::INVALID_PRICE);
}

TEST_CASE("spec fields of 0 mean no rule") {
    SymbolSpec none;
    none.symbol = "X";
    CHECK_FALSE(reason(*make_order("A", "X", OrderType::LIMIT, Side::BUY, 123.45678901, 0.00000007), &none).has_value());
}

TEST_CASE("notional comparison is exact and does not overflow") {
    const int64_t max = std::numeric_limits<int64_t>::max();
    CHECK(notional_at_least(max, max, max));          // ~8.5e37 in 1e-16 units, far beyond 64 bits
    CHECK(notional_at_least(fx(100.0), fx(0.05), fx(5.0)));
    CHECK_FALSE(notional_at_least(fx(100.0), fx(0.05) - 1, fx(5.0)));  // one unit short
    CHECK(notional_at_least(1, 1, 0));                // no minimum
    CHECK_FALSE(notional_at_least(0, fx(1.0), 1));
    // 0.00000001 x 500000000 = 5 exactly
    CHECK(notional_at_least(1, fx(500000000.0), fx(5.0)));
    CHECK_FALSE(notional_at_least(1, fx(500000000.0) - 1, fx(5.0)));  // one unit short (not representable as a double)
}

TEST_CASE_FIXTURE(EngineFixture, "D6 fixed: a sell limit without a price is rejected instead of sweeping the bids") {
    const std::string s = "VAL-D6";
    submit("B1", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    submit("B2", s, OrderType::LIMIT, Side::BUY, 50.0, 1.0);

    // In 1.0 a missing price became 0, so this sold into every bid and then rested an ask at 0
    for (OrderType type : {OrderType::LIMIT, OrderType::IOC, OrderType::FOK}) {
        CAPTURE(orderTypeToString(type));
        std::vector<ExecReport> reports;
        auto trades = engine().submit_order(make_order("S", s, type, Side::SELL, 0.0, 5.0), &reports);
        CHECK(trades.empty());
        REQUIRE(reports.size() == 1U);
        CHECK(reports[0].type == ExecType::REJECTED);
        CHECK(reports[0].reason == ExecReason::INVALID_PRICE);
    }
    nlohmann::json d = engine().get_l2_depth(s, 10);
    CHECK(d["bids"].size() == 2U);
    CHECK(d["asks"].empty());
}

TEST_CASE_FIXTURE(EngineFixture, "specs apply to existing and new books and survive reset") {
    submit("A", "BTC-USDT", OrderType::LIMIT, Side::BUY, 77000.005, 1.0);  // no spec yet: accepted
    CHECK(engine().has_symbol("BTC-USDT"));

    engine().set_symbol_spec(btc_spec());
    CHECK(last_report(engine(), make_order("B", "BTC-USDT", OrderType::LIMIT, Side::BUY, 77000.005, 1.0)).reason ==
          ExecReason::PRICE_TICK);
    CHECK(last_report(engine(), make_order("C", "BTC-USDT", OrderType::LIMIT, Side::BUY, 77000.01, 1.0)).type ==
          ExecType::ACK);

    engine().reset();
    CHECK_FALSE(engine().has_symbol("BTC-USDT"));
    REQUIRE(engine().symbol_spec("BTC-USDT").has_value());
    CHECK(engine().symbol_spec("BTC-USDT")->tick_size == fx(0.01));
    CHECK(last_report(engine(), make_order("D", "BTC-USDT", OrderType::LIMIT, Side::BUY, 1.0, 1.0)).reason ==
          ExecReason::MIN_NOTIONAL);  // the new book got the spec too

    // Other symbols are unaffected
    CHECK(last_report(engine(), make_order("E", "ETH-USDT", OrderType::LIMIT, Side::BUY, 1.0, 1.0)).type ==
          ExecType::ACK);
}

TEST_CASE_FIXTURE(EngineFixture, "an invalid modify is rejected and leaves the order unchanged") {
    engine().set_symbol_spec(btc_spec());
    submit("A", "BTC-USDT", OrderType::LIMIT, Side::BUY, 77000.0, 0.5);

    std::vector<ExecReport> reports;
    std::vector<Trade> trades;
    CHECK_FALSE(engine().modify_order("BTC-USDT", "A", fx(0.5), fx(77000.005), trades, &reports));
    REQUIRE(reports.size() == 1U);
    CHECK(reports[0].type == ExecType::REJECTED);
    CHECK(reports[0].reason == ExecReason::PRICE_TICK);
    CHECK(reports[0].order_id == "A");

    reports.clear();
    CHECK_FALSE(engine().modify_order("BTC-USDT", "A", fx(0.000015), fx(77000.0), trades, &reports));
    CHECK(reports.back().reason == ExecReason::QUANTITY_LOT);

    nlohmann::json bids = engine().get_l2_depth("BTC-USDT", 10)["bids"];
    REQUIRE(bids.size() == 1U);
    CHECK(bids[0][0] == "77000.000000");
    CHECK(bids[0][1] == "0.500000");

    // A valid modify still works
    reports.clear();
    CHECK(engine().modify_order("BTC-USDT", "A", fx(0.25), fx(77000.0), trades, &reports));
    CHECK(reports.back().type == ExecType::AMENDED);
}

TEST_CASE_FIXTURE(EngineFixture, "D18 fixed: read-only queries do not create books") {
    CHECK(engine().get_bbo("NOPE")["bid_price"] == "0.0");
    CHECK(engine().get_l2_depth("NOPE", 10)["bids"].empty());
    CHECK(engine().bbo("NOPE").bid_qty == 0);
    CHECK(engine().check_invariants("NOPE").empty());
    CHECK_FALSE(engine().is_crossed("NOPE"));
    CHECK_FALSE(engine().cancel_order("NOPE", "X"));
    CHECK_FALSE(engine().has_symbol("NOPE"));
}

} // TEST_SUITE("validation")
