// Fixed-point prices and quantities (P1.6).
#include <doctest.h>
#include "test_helpers.hpp"
#include <limits>

TEST_SUITE("types") {

TEST_CASE("parse_fixed: plain and Binance-style decimals are exact") {
    CHECK(parse_fixed("101") == 10100000000LL);
    CHECK(parse_fixed("101.5") == 10150000000LL);
    CHECK(parse_fixed("0.5") == 50000000LL);
    CHECK(parse_fixed(".5") == 50000000LL);
    CHECK(parse_fixed("5.") == 500000000LL);
    CHECK(parse_fixed("+2") == 200000000LL);
    CHECK(parse_fixed("0") == 0LL);
    CHECK(parse_fixed("00012.30") == 1230000000LL);
    CHECK(parse_fixed("68000.01000000") == 6800001000000LL); // Binance REST/WS format
    CHECK(parse_fixed("0.00000001") == 1LL);                 // smallest unit
    CHECK(parse_fixed("1.234567890000") == 123456789LL);      // extra trailing zeros are fine
}

TEST_CASE("parse_fixed: exponent notation (Python str() of small floats)") {
    CHECK(parse_fixed("1e-05") == 1000LL);
    CHECK(parse_fixed("1E-8") == 1LL);
    CHECK(parse_fixed("2.5e3") == 250000000000LL);
    CHECK(parse_fixed("1e+2") == 10000000000LL);
}

TEST_CASE("parse_fixed: rejects malformed, negative, too precise and out-of-range input") {
    for (const char* bad : {"", ".", "-1", "-0.5", "abc", "1.2.3", "1,5", " 1", "1 ", "1e", "1e-", "1ex", "nan", "inf",
                            "0.000000001", "1e-9", "1.000000005", "1e200", "92233720368.54775808"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_fixed(bad).has_value());
    }
}

TEST_CASE("parse_fixed: largest representable value") {
    CHECK(parse_fixed("92233720368.54775807") == std::numeric_limits<int64_t>::max());
}

TEST_CASE("format_fixed: exact rendering with trimmed zeros") {
    CHECK(format_fixed(10150000000LL) == "101.5");
    CHECK(format_fixed(10100000000LL) == "101.0");
    CHECK(format_fixed(1LL) == "0.00000001");
    CHECK(format_fixed(0LL) == "0.0");
    CHECK(format_fixed(-50000000LL) == "-0.5");
    CHECK(format_fixed(6800001000000LL) == "68000.01");
    CHECK(format_fixed(std::numeric_limits<int64_t>::min()) == "-92233720368.54775808");
}

TEST_CASE("format_fixed and parse_fixed round-trip") {
    for (int64_t v : {0LL, 1LL, 99LL, 100000000LL, 123456789012LL, 6800001000000LL}) {
        CAPTURE(v);
        CHECK(parse_fixed(format_fixed(v)) == v);
    }
}

TEST_CASE("format_legacy keeps the 1.0 wire format") {
    CHECK(format_legacy(10000000000LL) == "100.000000");
    CHECK(format_legacy(150000000LL) == "1.500000");
}

TEST_CASE_FIXTURE(EngineFixture, "D5 fixed: equal prices from different representations share one level") {
    const std::string s = "TY-D5";
    // 0.1 + 0.2 is 0.30000000000000004 in binary floating point; in 1.0 these became two levels
    auto a = make_order("A", s, OrderType::LIMIT, Side::BUY, 0.1 + 0.2, 1.0);
    auto b = make_order("B", s, OrderType::LIMIT, Side::BUY, 0.0, 1.0);
    b->price = *parse_fixed("0.3");
    engine().submit_order(a);
    engine().submit_order(b);

    nlohmann::json d = engine().get_l2_depth(s, 10);
    REQUIRE(d["bids"].size() == 1U);
    CHECK(d["bids"][0][1].get<std::string>() == "2.000000");
}

} // TEST_SUITE("types")
