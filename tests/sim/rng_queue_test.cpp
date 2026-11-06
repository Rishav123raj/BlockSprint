// Deterministic RNG (P3.2) and event queue (P3.3).
#include <doctest.h>
#include "event_queue.hpp"
#include "rng.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace sim;

TEST_SUITE("rng") {

TEST_CASE("xoshiro256** seeded by SplitMix64 matches the reference algorithm") {
    // Expected values from an independent Python implementation of SplitMix64 + xoshiro256** (seed 42).
    // The same numbers on every compiler and platform are what makes simulations reproducible.
    Rng rng(42);
    CHECK(rng.next() == 0x15780b2e0c2ec716ULL);
    CHECK(rng.next() == 0x6104d9866d113a7eULL);
    CHECK(rng.next() == 0xae17533239e499a1ULL);
    CHECK(rng.next() == 0xecb8ad4703b360a1ULL);
    CHECK(rng.next() == 0xfde6dc7fe2ec5e64ULL);
}

TEST_CASE("same seed, same sequence; different seeds differ") {
    Rng a(7), b(7), c(8);
    bool differs = false;
    bool same = true;
    for (int i = 0; i < 1000; ++i) {
        uint64_t x = a.next();
        same &= x == b.next();
        differs |= x != c.next();
    }
    CHECK(same);
    CHECK(differs);
}

TEST_CASE("uniform is in [0, 1) and below(n) is in range and roughly flat") {
    Rng rng(1);
    std::vector<int> counts(10, 0);
    bool in_range = true;
    for (int i = 0; i < 100000; ++i) {
        double u = rng.uniform();
        in_range &= u >= 0.0 && u < 1.0;
        uint64_t k = rng.below(10);
        in_range &= k < 10;
        ++counts[k < 10 ? k : 0];
    }
    CHECK(in_range);
    for (int c : counts) {
        CHECK(c > 9500);
        CHECK(c < 10500);
    }
    CHECK_THROWS_AS(rng.below(0), std::invalid_argument);
}

TEST_CASE("det_log matches std::log to 1e-15 relative and rejects non-positive input") {
    for (double x : {1e-300, 1e-9, 0.001, 0.5, 0.7071, 0.99999, 1.0, 1.0000001, 2.0, 3.14159, 1e6, 1e300}) {
        CAPTURE(x);
        double expected = std::log(x);
        double got = det_log(x);
        CHECK(std::fabs(got - expected) <= 1e-15 * std::fmax(1.0, std::fabs(expected)));
    }
    CHECK(det_log(1.0) == 0.0);
    CHECK_THROWS_AS(det_log(0.0), std::domain_error);
    CHECK_THROWS_AS(det_log(-1.0), std::domain_error);
}

TEST_CASE("exponential and normal have the right moments") {
    Rng rng(123);
    const int n = 200000;
    double sum_e = 0, sum_n = 0, sum_n2 = 0;
    for (int i = 0; i < n; ++i) {
        sum_e += rng.exponential(2.0);
        double z = rng.normal();
        sum_n += z;
        sum_n2 += z * z;
    }
    CHECK(sum_e / n == doctest::Approx(2.0).epsilon(0.02));
    CHECK(sum_n / n == doctest::Approx(0.0).epsilon(0.01));
    CHECK(sum_n2 / n == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("empirical distribution interpolates its quantiles") {
    EmpiricalDistribution d({30.0, 10.0, 20.0});
    CHECK(d.quantile(0.0) == 10.0);
    CHECK(d.quantile(0.5) == 20.0);
    CHECK(d.quantile(0.75) == 25.0);
    CHECK(d.quantile(1.0) == 30.0);
    Rng rng(5);
    bool in_range = true;
    for (int i = 0; i < 1000; ++i) {
        double v = d.sample(rng);
        in_range &= v >= 10.0 && v <= 30.0;
    }
    CHECK(in_range);
    CHECK_THROWS_AS(EmpiricalDistribution({}), std::invalid_argument);
}

} // TEST_SUITE("rng")

TEST_SUITE("event queue") {

TEST_CASE("events come out by time, then class, then insertion order") {
    EventQueue<std::string> q;
    q.push(200, EventClass::MARKET_DATA, "md@200");
    q.push(100, EventClass::TIMER, "timer@100");
    q.push(100, EventClass::ORDER_ARRIVE, "order@100 first");
    q.push(100, EventClass::MARKET_DATA, "md@100");
    q.push(100, EventClass::ORDER_ARRIVE, "order@100 second");
    q.push(50, EventClass::STRATEGY_EXEC, "exec@50");
    std::vector<std::string> order;
    while (!q.empty()) {
        order.push_back(q.pop().payload);
    }
    CHECK(order == std::vector<std::string>{"exec@50", "md@100", "order@100 first", "order@100 second",
                                            "timer@100", "md@200"});
    CHECK_THROWS_AS(q.pop(), std::out_of_range);
}

TEST_CASE("push returns increasing sequence numbers") {
    EventQueue<int> q;
    CHECK(q.push(5, EventClass::TIMER, 1) == 0);
    CHECK(q.push(1, EventClass::TIMER, 2) == 1);
    CHECK(q.top().payload == 2);
    CHECK(q.size() == 2);
}

} // TEST_SUITE("event queue")
