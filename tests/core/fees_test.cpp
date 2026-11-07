// Trading fees (P1.13).
#include <doctest.h>
#include "test_helpers.hpp"
#include <limits>

namespace {

int64_t rate(double fraction) { return fx(fraction); }

std::vector<ExecReport> fills_of(const std::vector<ExecReport>& reports) {
    std::vector<ExecReport> out;
    for (const auto& r : reports) {
        if (r.type == ExecType::FILL) {
            out.push_back(r);
        }
    }
    return out;
}

} // namespace

TEST_SUITE("fees") {

TEST_CASE("fee_for is exact on realistic fills") {
    // 0.001 BTC at 77000.01 = 77.00001 USDT; 0.10% of that = 0.07700001 USDT
    CHECK(fee_for(fx(77000.01), fx(0.001), rate(0.001)) == 7700001);
    // 2.5 ETH at 2292.48 = 5731.2 USDT; 0.075% (BNB discount) = 4.29840 USDT
    CHECK(fee_for(fx(2292.48), fx(2.5), rate(0.00075)) == 429840000);
    CHECK(fee_for(fx(100.0), fx(1.0), 0) == 0);
    CHECK(fee_for(0, fx(1.0), rate(0.001)) == 0);
}

TEST_CASE("fee_for rounds half away from zero, to 1e-8") {
    // 1.00000001 x 0.00000001 = 1.00000001e-8: notional rounds to 1 unit; 50% of 1 unit = 0.5 -> 1
    CHECK(fee_for(fx(1.00000001), 1, rate(0.5)) == 1);
    // 0.4 units rounds to 0
    CHECK(fee_for(fx(1.0), 1, rate(0.4)) == 0);
    // Rebates round the same way, with a negative sign
    CHECK(fee_for(fx(1.00000001), 1, -rate(0.5)) == -1);
    CHECK(fee_for(fx(100.0), fx(1.0), -rate(0.00005)) == -500000);  // -0.005% of 100 = -0.005
}

TEST_CASE("fee_for does not overflow on extreme values") {
    const int64_t max = std::numeric_limits<int64_t>::max();
    // Largest realistic fill: Binance's max BTC order (9000) at 1,000,000 USDT = 9e9 USDT; 0.10% = 9e6
    CHECK(fee_for(fx(1000000.0), fx(9000.0), rate(0.001)) == fx(9000000.0));
    // Notionals above ~92 billion quote units per fill saturate instead of wrapping
    CHECK(fee_for(max, max, rate(1.0)) == max);
    CHECK(fee_for(max, max, -rate(1.0)) == -max);
}

TEST_CASE("notional_for: exact on realistic fills, rounded half away from zero, saturating") {
    CHECK(notional_for(fx(77000.01), fx(0.001)) == fx(77.00001));
    CHECK(notional_for(fx(2292.48), fx(2.5)) == fx(5731.2));
    CHECK(notional_for(fx(1.5), 1) == 2);   // 1.5e-8 -> 2e-8
    CHECK(notional_for(fx(1.4), 1) == 1);
    CHECK(notional_for(0, fx(1.0)) == 0);
    const int64_t max = std::numeric_limits<int64_t>::max();
    CHECK(notional_for(max, max) == max);
}

TEST_CASE("report_market_data_liquidity = false: no reports for synthetic orders, the participant's still built") {
    MatchingEngine eng(std::make_shared<SimClock>(5), EngineOptions{false, false});
    std::vector<ExecReport> seen;
    eng.register_exec_report_callback([&](const ExecReport& r) { seen.push_back(r); });
    auto synthetic = make_order("~1", "X", OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    synthetic->owner_id = "MARKET";
    synthetic->market_data_liquidity = true;
    eng.submit_order(synthetic);
    CHECK(seen.empty());
    auto taker = make_order("T", "X", OrderType::IOC, Side::BUY, 100.0, 0.4);
    taker->owner_id = "S1";
    eng.submit_order(taker);
    REQUIRE(seen.size() == 2);  // the taker's ACK and FILL; not the synthetic maker's FILL
    CHECK(seen[0].type == ExecType::ACK);
    CHECK(seen[1].type == ExecType::FILL);
    CHECK(seen[1].order_id == "T");
    // Sequence numbers still count the skipped reports: synthetic ACK 1, taker ACK 2, taker FILL 3, synthetic FILL 4
    CHECK(seen[0].seq == 2);
    CHECK(seen[1].seq == 3);
}

TEST_CASE("a new engine charges no fees") {
    MatchingEngine eng;
    CHECK(eng.fee_schedule("anyone").maker_rate == 0);
    std::vector<ExecReport> r;
    eng.submit_order(make_order("M", "X", OrderType::LIMIT, Side::SELL, 100.0, 1.0), &r);
    eng.submit_order(make_order("T", "X", OrderType::MARKET, Side::BUY, 0.0, 1.0), &r);
    for (const auto& f : fills_of(r)) {
        CHECK(f.fee == 0);
        CHECK(f.cum_fee == 0);
    }
}

TEST_CASE("maker and taker pay their own rates, per owner") {
    MatchingEngine eng;
    eng.set_default_fee_schedule(binance_spot_regular());         // 0.10% / 0.10%
    eng.set_fee_schedule("vip", FeeSchedule{rate(0.0002), rate(0.0004)});
    eng.set_fee_schedule("mm", FeeSchedule{-rate(0.00005), rate(0.0003)});  // maker rebate

    auto maker = make_order("M", "X", OrderType::LIMIT, Side::SELL, 100.0, 2.0);
    maker->owner_id = "mm";
    eng.submit_order(maker);

    auto taker = make_order("T", "X", OrderType::LIMIT, Side::BUY, 100.0, 1.0);
    taker->owner_id = "vip";
    std::vector<ExecReport> r;
    eng.submit_order(taker, &r);
    auto fills = fills_of(r);
    REQUIRE(fills.size() == 2U);
    CHECK(fills[0].order_id == "T");
    CHECK(fills[0].fee == fx(0.04));     // 0.04% of 100
    CHECK(fills[1].order_id == "M");
    CHECK(fills[1].fee == -fx(0.005));   // -0.005% of 100: a rebate

    // An owner without a schedule pays the default
    std::vector<ExecReport> r2;
    eng.submit_order(make_order("T2", "X", OrderType::LIMIT, Side::BUY, 100.0, 1.0), &r2);
    auto fills2 = fills_of(r2);
    REQUIRE(fills2.size() == 2U);
    CHECK(fills2[0].fee == fx(0.1));     // 0.10% of 100
    CHECK(eng.fee_schedule("nobody").taker_rate == rate(0.001));
}

TEST_CASE("cumulative fees add up across fills and carry through modify and cancel") {
    MatchingEngine eng;
    eng.set_default_fee_schedule(binance_spot_regular());
    eng.submit_order(make_order("A", "X", OrderType::LIMIT, Side::BUY, 100.0, 3.0));
    eng.submit_order(make_order("S1", "X", OrderType::LIMIT, Side::SELL, 100.0, 1.0));  // A fills 1 as maker
    eng.submit_order(make_order("S2", "X", OrderType::LIMIT, Side::SELL, 101.0, 1.0));

    // Re-pricing A through 101 makes it a taker for 1 more
    std::vector<ExecReport> r;
    std::vector<Trade> t;
    CHECK(eng.modify_order("X", "A", fx(2.0), fx(101.0), t, &r));
    CHECK(r[0].type == ExecType::AMENDED);
    CHECK(r[0].cum_fee == fx(0.1));            // carried over from the first fill
    auto fills = fills_of(r);
    REQUIRE(fills.size() == 2U);
    CHECK(fills[0].order_id == "A");
    CHECK(fills[0].fee == fx(0.101));          // 0.10% of 101
    CHECK(fills[0].cum_fee == fx(0.201));

    std::vector<ExecReport> c;
    CHECK(eng.cancel_order("X", "A", &c));
    CHECK(c[0].cum_fee == fx(0.201));
}

TEST_CASE("market-data liquidity pays no fees; its counterparty still does") {
    MatchingEngine eng;
    eng.set_default_fee_schedule(binance_spot_regular());
    auto level = make_order("L", "X", OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    level->owner_id = "MARKET";
    level->market_data_liquidity = true;
    eng.submit_order(level);

    std::vector<ExecReport> r;
    eng.submit_order(make_order("T", "X", OrderType::MARKET, Side::BUY, 0.0, 1.0), &r);
    auto fills = fills_of(r);
    REQUIRE(fills.size() == 2U);
    CHECK(fills[0].fee == fx(0.1));   // the user, as taker
    CHECK(fills[1].fee == 0);         // the mirrored level
}

TEST_CASE("fee settings survive reset; trades stay fee-free (fees are private)") {
    MatchingEngine eng;
    eng.set_default_fee_schedule(binance_spot_regular());
    eng.reset();
    CHECK(eng.fee_schedule("x").taker_rate == rate(0.001));

    eng.submit_order(make_order("M", "X", OrderType::LIMIT, Side::SELL, 100.5, 1.0));
    std::vector<ExecReport> r;
    auto trades = eng.submit_order(make_order("T", "X", OrderType::MARKET, Side::BUY, 0.0, 1.0), &r);
    REQUIRE(trades.size() == 1U);
    nlohmann::json trade_json = trades[0];
    CHECK_FALSE(trade_json.contains("fee"));

    nlohmann::json fill_json = fills_of(r)[0];
    CHECK(fill_json["fee"] == "0.1005");
    CHECK(fill_json["cum_fee"] == "0.1005");
    nlohmann::json ack_json = r[0];
    CHECK(ack_json["cum_fee"] == "0.0");
    CHECK_FALSE(ack_json.contains("fee"));
}

} // TEST_SUITE("fees")
