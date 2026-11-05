// Post-only orders (LIMIT_MAKER) and Binance-style amend (P1.14, fixes D11).
#include <doctest.h>
#include "test_helpers.hpp"
#include <utility>
#include <vector>

namespace {

using Levels = std::vector<std::pair<double, double>>;

Levels side_of(MatchingEngine& eng, const std::string& s, const char* side) {
    Levels levels;
    // Keep the JSON alive: ranging over part of a temporary would read destroyed memory
    const nlohmann::json depth = eng.get_l2_depth(s, 100);
    for (const auto& lvl : depth[side]) {
        levels.emplace_back(std::stod(lvl[0].get<std::string>()), std::stod(lvl[1].get<std::string>()));
    }
    return levels;
}

ExecReport last(const std::vector<ExecReport>& r) {
    REQUIRE_FALSE(r.empty());
    return r.back();
}

} // namespace

TEST_SUITE("post-only") {

TEST_CASE("order type names") {
    CHECK(stringToOrderType("limit_maker") == OrderType::LIMIT_MAKER);
    CHECK(stringToOrderType("post_only") == OrderType::LIMIT_MAKER);
    CHECK(orderTypeToString(OrderType::LIMIT_MAKER) == "limit_maker");
}

TEST_CASE_FIXTURE(EngineFixture, "a post-only order that does not cross rests and later fills as maker") {
    const std::string s = "PO";
    engine().set_default_fee_schedule(FeeSchedule{fx(0.0002), fx(0.001)});
    submit("A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);

    std::vector<ExecReport> r;
    auto trades = engine().submit_order(make_order("P", s, OrderType::LIMIT_MAKER, Side::BUY, 100.0, 2.0), &r);
    CHECK(trades.empty());
    REQUIRE(r.size() == 1U);
    CHECK(r[0].type == ExecType::ACK);
    CHECK(r[0].order_type == OrderType::LIMIT_MAKER);
    CHECK(side_of(engine(), s, "bids") == Levels{{100.0, 2.0}});

    std::vector<ExecReport> r2;
    engine().submit_order(make_order("T", s, OrderType::MARKET, Side::SELL, 0.0, 1.0), &r2);
    REQUIRE(r2.size() == 3U);
    CHECK(r2[2].order_id == "P");
    CHECK(r2[2].liquidity == Liquidity::MAKER);
    CHECK(r2[2].fee == fx(0.02));  // maker rate 0.02% of 100
}

TEST_CASE_FIXTURE(EngineFixture, "a post-only order that would trade is rejected and the book is untouched") {
    const std::string s = "PO-REJ";
    submit("A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("B", s, OrderType::LIMIT, Side::BUY, 99.0, 1.0);

    for (double price : {101.0, 102.0}) {  // touching the best ask counts: it would take
        CAPTURE(price);
        std::vector<ExecReport> r;
        CHECK(engine().submit_order(make_order("P", s, OrderType::LIMIT_MAKER, Side::BUY, price, 1.0), &r).empty());
        REQUIRE(r.size() == 1U);
        CHECK(r[0].type == ExecType::REJECTED);
        CHECK(r[0].reason == ExecReason::POST_ONLY_WOULD_TAKE);
        CHECK(r[0].text.find("would trade immediately against the best ask") != std::string::npos);
    }
    std::vector<ExecReport> r;
    engine().submit_order(make_order("Q", s, OrderType::LIMIT_MAKER, Side::SELL, 99.0, 1.0), &r);
    CHECK(last(r).reason == ExecReason::POST_ONLY_WOULD_TAKE);

    CHECK(side_of(engine(), s, "asks") == Levels{{101.0, 1.0}});
    CHECK(side_of(engine(), s, "bids") == Levels{{99.0, 1.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "post-only is rejected even when the crossing order is its own") {
    // On Binance a LIMIT_MAKER that would match is rejected before self-trade prevention applies
    const std::string s = "PO-OWN";
    submit_as("A", StpPolicy::SKIP, "OWN", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    std::vector<Trade> none;
    auto p = make_order("P", s, OrderType::LIMIT_MAKER, Side::BUY, 101.0, 1.0);
    p->owner_id = "A";
    p->stp_policy = StpPolicy::SKIP;
    std::vector<ExecReport> r;
    engine().submit_order(p, &r);
    CHECK(last(r).reason == ExecReason::POST_ONLY_WOULD_TAKE);
}

TEST_CASE_FIXTURE(EngineFixture, "a post-only order without a price is rejected by validation") {
    std::vector<ExecReport> r;
    engine().submit_order(make_order("P", "PO-NP", OrderType::LIMIT_MAKER, Side::BUY, 0.0, 1.0), &r);
    CHECK(last(r).reason == ExecReason::INVALID_PRICE);
}

} // TEST_SUITE("post-only")

TEST_SUITE("amend") {

// A buy for 5.0 at 100 with 2.0 already filled: open quantity 3.0
struct PartlyFilled : EngineFixture {
    const std::string s = "AM";
    PartlyFilled() {
        submit("A", s, OrderType::LIMIT, Side::BUY, 100.0, 5.0);
        submit("Z", s, OrderType::LIMIT, Side::BUY, 100.0, 1.0);   // queued behind A
        submit("S", s, OrderType::LIMIT, Side::SELL, 100.0, 2.0);  // fills 2.0 of A
    }
};

TEST_CASE_FIXTURE(PartlyFilled, "D11 fixed: amend takes the new TOTAL quantity; modify keeps the 1.0 open-quantity meaning") {
    std::vector<ExecReport> r;
    std::vector<Trade> t;
    CHECK(engine().amend_order(s, "A", fx(4.0), std::nullopt, t, &r));
    REQUIRE(r.size() == 1U);
    CHECK(r[0].type == ExecType::AMENDED);
    CHECK(r[0].quantity == fx(4.0));     // new total
    CHECK(r[0].cum_qty == fx(2.0));
    CHECK(r[0].leaves_qty == fx(2.0));   // 4 total - 2 filled
    CHECK(r[0].price == fx(100.0));      // no price given: unchanged

    // The same number through modify means "open quantity 3.5" (total 5.5)
    r.clear();
    CHECK(engine().modify_order(s, "A", fx(3.5), fx(100.0), t, &r));
    CHECK(r.back().leaves_qty == fx(3.5));
    CHECK(r.back().quantity == fx(5.5));
}

TEST_CASE_FIXTURE(PartlyFilled, "amend down at the same price keeps queue priority") {
    std::vector<Trade> t;
    CHECK(engine().amend_order(s, "A", fx(3.0), fx(100.0), t));  // open 3 -> 1
    auto trades = submit("T", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].maker_order_id == "A");  // still ahead of Z
}

TEST_CASE_FIXTURE(PartlyFilled, "amend up loses queue priority") {
    std::vector<Trade> t;
    CHECK(engine().amend_order(s, "A", fx(6.0), std::nullopt, t));  // open 3 -> 4
    auto trades = submit("T", s, OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].maker_order_id == "Z");
}

TEST_CASE_FIXTURE(PartlyFilled, "amend to a new price re-queues and can execute") {
    submit("X", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    std::vector<ExecReport> r;
    std::vector<Trade> t;
    CHECK(engine().amend_order(s, "A", fx(5.0), fx(101.0), t, &r));
    REQUIRE(t.size() == 1U);
    CHECK(t[0].maker_order_id == "X");
    CHECK(r[0].type == ExecType::AMENDED);
    CHECK(r[1].type == ExecType::FILL);
    CHECK(r[1].cum_qty == fx(3.0));
    CHECK(r[1].leaves_qty == fx(2.0));
}

TEST_CASE_FIXTURE(PartlyFilled, "amend to the filled quantity or less cancels the rest") {
    for (double total : {2.0, 1.0}) {
        CAPTURE(total);
        submit("C" + std::to_string(int(total)), s, OrderType::LIMIT, Side::BUY, 90.0, 5.0);
    }
    std::vector<ExecReport> r;
    std::vector<Trade> t;
    CHECK(engine().amend_order(s, "A", fx(2.0), std::nullopt, t, &r));
    REQUIRE(r.size() == 1U);
    CHECK(r[0].type == ExecType::CANCELLED);
    CHECK(r[0].reason == ExecReason::USER);
    CHECK(r[0].cum_qty == fx(2.0));
    CHECK(side_of(engine(), s, "bids") == Levels{{100.0, 1.0}, {90.0, 10.0}});
}

TEST_CASE_FIXTURE(EngineFixture, "amending a post-only order to a crossing price is rejected; it stays as it was") {
    const std::string s = "AM-PO";
    submit("ASK", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    submit("P", s, OrderType::LIMIT_MAKER, Side::BUY, 100.0, 1.0);

    std::vector<ExecReport> r;
    std::vector<Trade> t;
    CHECK_FALSE(engine().amend_order(s, "P", fx(1.0), fx(101.0), t, &r));
    REQUIRE(r.size() == 1U);
    CHECK(r[0].type == ExecType::REJECTED);
    CHECK(r[0].reason == ExecReason::POST_ONLY_WOULD_TAKE);
    CHECK(t.empty());
    CHECK(side_of(engine(), s, "bids") == Levels{{100.0, 1.0}});

    // A non-crossing re-price is fine and keeps the order post-only
    CHECK(engine().amend_order(s, "P", fx(1.0), fx(100.5), t));
    r.clear();
    CHECK_FALSE(engine().amend_order(s, "P", fx(1.0), fx(102.0), t, &r));
    CHECK(last(r).reason == ExecReason::POST_ONLY_WOULD_TAKE);
}

TEST_CASE_FIXTURE(EngineFixture, "amend of an unknown order reports nothing") {
    std::vector<ExecReport> r;
    std::vector<Trade> t;
    CHECK_FALSE(engine().amend_order("AM-NONE", "X", fx(1.0), std::nullopt, t, &r));
    CHECK(r.empty());
}

TEST_CASE_FIXTURE(EngineFixture, "amend follows the symbol's trading rules") {
    SymbolSpec spec;
    spec.symbol = "AM-SPEC";
    spec.tick_size = fx(0.01);
    engine().set_symbol_spec(spec);
    submit("A", spec.symbol, OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    std::vector<ExecReport> r;
    std::vector<Trade> t;
    CHECK_FALSE(engine().amend_order(spec.symbol, "A", fx(1.0), fx(100.005), t, &r));
    CHECK(last(r).reason == ExecReason::PRICE_TICK);
}

} // TEST_SUITE("amend")
