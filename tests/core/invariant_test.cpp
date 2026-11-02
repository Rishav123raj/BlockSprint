// Book invariants (P1.5): every internal structure stays consistent after every operation.
#include <doctest.h>
#include "workload.hpp"

TEST_SUITE("invariants") {

TEST_CASE_FIXTURE(EngineFixture, "book invariants hold after every operation of random workloads") {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        const std::string symbol = "INV-" + std::to_string(seed);
        size_t violations_seen = 0;
        run_random_workload(engine(), symbol, seed, 2000, [&](int op) {
            auto violations = engine().check_invariants(symbol);
            if (!violations.empty() && violations_seen == 0) {
                // Report only the first broken operation per seed to keep the output readable
                FAIL_CHECK("seed " << seed << ", op " << op << ": " << violations.front());
            }
            violations_seen += violations.size();
        });
        CHECK_MESSAGE(violations_seen == 0, "seed " << seed);
    }
}

TEST_CASE_FIXTURE(EngineFixture, "without self-trade prevention the book never crosses") {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        const std::string symbol = "INV-NOSTP-" + std::to_string(seed);
        int crossed_ops = 0;
        run_random_workload(
            engine(), symbol, seed, 2000, [&](int) { crossed_ops += engine().is_crossed(symbol); }, 0);
        CHECK_MESSAGE(crossed_ops == 0, "seed " << seed);
    }
}

TEST_CASE_FIXTURE(EngineFixture, "invariants hold on an empty book") {
    CHECK(engine().check_invariants("INV-EMPTY").empty());
    CHECK_FALSE(engine().is_crossed("INV-EMPTY"));
}

TEST_CASE_FIXTURE(EngineFixture, "is_crossed reports the D7 crossed book") {
    const std::string s = "INV-D7";
    submit("BINANCE_A", s, OrderType::LIMIT, Side::SELL, 101.0, 1.0);
    CHECK_FALSE(engine().is_crossed(s));
    submit("BINANCE_B", s, OrderType::LIMIT, Side::BUY, 102.0, 1.0);
    CHECK(engine().is_crossed(s));
    // A crossed book is still internally consistent
    CHECK(engine().check_invariants(s).empty());
}

} // TEST_SUITE("invariants")
