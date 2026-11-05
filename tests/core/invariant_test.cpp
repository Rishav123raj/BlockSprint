// Book invariants (P1.5): every internal structure stays consistent after every operation.
// Since P1.9 a crossed book is also a violation.
#include <doctest.h>
#include "workload.hpp"

namespace {

// Runs a workload and checks invariants after every operation; reports the first violation per seed
void check_every_operation(MatchingEngine& eng, uint64_t seed, int ops, uint32_t stp_percent) {
    const std::string symbol = "INV-" + std::to_string(stp_percent) + "-" + std::to_string(seed);
    size_t violations_seen = 0;
    run_random_workload(
        eng, symbol, seed, ops,
        [&](int op) {
            auto violations = eng.check_invariants(symbol);
            if (!violations.empty() && violations_seen == 0) {
                FAIL_CHECK("seed " << seed << ", op " << op << ": " << violations.front());
            }
            violations_seen += violations.size();
        },
        stp_percent);
    CHECK_MESSAGE(violations_seen == 0, "seed " << seed);
}

} // namespace

TEST_SUITE("invariants") {

TEST_CASE_FIXTURE(EngineFixture, "book invariants hold after every operation of random workloads") {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        check_every_operation(engine(), seed, 2000, 15);
    }
}

TEST_CASE_FIXTURE(EngineFixture, "the book never crosses, even when half the orders are mirrored liquidity") {
    // Mirrored orders skip each other, which is how 1.0 produced crossed books (D7, D21)
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        check_every_operation(engine(), seed, 2000, 50);
    }
}

TEST_CASE_FIXTURE(EngineFixture, "book invariants hold with every STP policy in play") {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        const std::string symbol = "INV-MIXED-" + std::to_string(seed);
        size_t violations_seen = 0;
        run_random_workload(
            engine(), symbol, seed, 2000,
            [&](int op) {
                auto violations = engine().check_invariants(symbol);
                if (!violations.empty() && violations_seen == 0) {
                    FAIL_CHECK("seed " << seed << ", op " << op << ": " << violations.front());
                }
                violations_seen += violations.size();
            },
            15, true, seed % 2 == 0);  // every other seed adds post-only orders and amends
        CHECK_MESSAGE(violations_seen == 0, "seed " << seed);
    }
}

TEST_CASE_FIXTURE(EngineFixture, "invariants hold on an empty book") {
    CHECK(engine().check_invariants("INV-EMPTY").empty());
    CHECK_FALSE(engine().is_crossed("INV-EMPTY"));
}

} // TEST_SUITE("invariants")
