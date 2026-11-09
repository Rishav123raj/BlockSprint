// Strategy registry and parameters (P3.14).
#include <doctest.h>
#include "strategies/params.hpp"
#include "strategies/registry.hpp"

#include <stdexcept>
#include <string>

using namespace sim;
using nlohmann::json;

namespace {

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

} // TEST_SUITE("strategies")
