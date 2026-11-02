// Instantiable engine (P1.8) and injected clock (P1.7).
#include <doctest.h>
#include "workload.hpp"
#include <thread>

namespace {

// Hash of every trade field, including the trade ID and timestamp the golden workload ignores
uint64_t full_trade_stream_hash(MatchingEngine& eng, SimClock& clock, uint64_t seed) {
    uint64_t hash = 14695981039346656037ULL;
    eng.register_trade_callback([&](const std::string&, const Trade& t) {
        hash = fnv1a(hash, t.trade_id + "@" + std::to_string(t.timestamp) + ":" + trade_key(t));
    });
    // One simulated millisecond per operation
    run_random_workload(eng, "DET", seed, 2000, [&](int) { clock.advance(1000000); });
    eng.register_trade_callback({});
    return hash;
}

} // namespace

TEST_SUITE("engine") {

TEST_CASE("engines are independent of each other") {
    MatchingEngine a;
    MatchingEngine b;
    a.submit_order(make_order("S", "BTC-USDT", OrderType::LIMIT, Side::SELL, 100.0, 1.0));

    // The same symbol in another engine is a different, empty book
    CHECK(b.submit_order(make_order("T", "BTC-USDT", OrderType::LIMIT, Side::BUY, 100.0, 1.0)).empty());
    CHECK(b.get_bbo("BTC-USDT")["ask_price"].get<std::string>() == "0.0");
    CHECK(a.get_bbo("BTC-USDT")["ask_price"].get<std::string>() == "100.000000");
}

TEST_CASE("trade IDs start at 1 per engine and are shared across its symbols") {
    MatchingEngine eng;
    eng.submit_order(make_order("S1", "X", OrderType::LIMIT, Side::SELL, 100.0, 1.0));
    eng.submit_order(make_order("S2", "Y", OrderType::LIMIT, Side::SELL, 100.0, 1.0));

    auto t1 = eng.submit_order(make_order("B1", "X", OrderType::MARKET, Side::BUY, 0.0, 1.0));
    auto t2 = eng.submit_order(make_order("B2", "Y", OrderType::MARKET, Side::BUY, 0.0, 1.0));
    REQUIRE(t1.size() == 1U);
    REQUIRE(t2.size() == 1U);
    CHECK(t1[0].trade_id == "1");
    CHECK(t2[0].trade_id == "2");

    // reset() clears the books but keeps IDs unique
    eng.reset();
    eng.submit_order(make_order("S3", "X", OrderType::LIMIT, Side::SELL, 100.0, 1.0));
    auto t3 = eng.submit_order(make_order("B3", "X", OrderType::MARKET, Side::BUY, 0.0, 1.0));
    REQUIRE(t3.size() == 1U);
    CHECK(t3[0].trade_id == "3");
}

TEST_CASE("trades and market data are timestamped by the injected clock") {
    auto clock = std::make_shared<SimClock>(1700000000000000000ULL); // 2023-11-14T22:13:20Z
    MatchingEngine eng(clock);

    eng.submit_order(make_order("S", "X", OrderType::LIMIT, Side::SELL, 100.0, 2.0));
    clock->advance(1500); // 1.5 microseconds
    auto trades = eng.submit_order(make_order("B", "X", OrderType::LIMIT, Side::BUY, 100.0, 1.0));
    REQUIRE(trades.size() == 1U);
    CHECK(trades[0].timestamp == 1700000000000001500ULL);
    CHECK(eng.clock().now_ns() == 1700000000000001500ULL);

    nlohmann::json trade_json = trades[0];
    CHECK(trade_json["timestamp"].get<std::string>() == "2023-11-14T22:13:20.000001Z");
    CHECK(eng.get_bbo("X")["timestamp"].get<std::string>() == "2023-11-14T22:13:20.000001Z");
    CHECK(eng.get_l2_depth("X")["timestamp"].get<std::string>() == "2023-11-14T22:13:20.000001Z");
}

TEST_CASE("SimClock never moves backwards") {
    SimClock clock(100);
    clock.set(100);
    clock.advance(5);
    CHECK(clock.now_ns() == 105U);
    CHECK_THROWS_AS(clock.set(104), std::invalid_argument);
}

TEST_CASE("engine requires a clock") {
    CHECK_THROWS_AS(MatchingEngine(nullptr), std::invalid_argument);
}

TEST_CASE("two simulated runs are bit-identical, including trade IDs and timestamps") {
    auto clock_a = std::make_shared<SimClock>(1000);
    auto clock_b = std::make_shared<SimClock>(1000);
    MatchingEngine a(clock_a);
    MatchingEngine b(clock_b);

    uint64_t hash_a = full_trade_stream_hash(a, *clock_a, 42);
    uint64_t hash_b = full_trade_stream_hash(b, *clock_b, 42);
    CHECK(hash_a == hash_b);
    CHECK(full_trade_stream_hash(a, *clock_a, 43) != hash_a); // sanity: the hash depends on the input
}

TEST_CASE("an engine without locking produces identical results") {
    MatchingEngine locked;
    MatchingEngine unlocked(std::make_shared<WallClock>(), EngineOptions{false});
    WorkloadStats x = run_random_workload(locked, "LOCK", 20261101ULL, 5000);
    WorkloadStats y = run_random_workload(unlocked, "LOCK", 20261101ULL, 5000);
    CHECK(x.hash == y.hash);
    CHECK(x.hash == 11732550826495055626ULL); // the v1.0.0 golden hash
}

TEST_CASE("a thread-safe engine stays consistent under concurrent use") {
    MatchingEngine eng;
    std::vector<std::thread> threads;
    for (uint64_t t = 0; t < 4; ++t) {
        threads.emplace_back([&eng, t] {
            // Each thread works its own symbol; trade IDs come from the shared engine sequence
            run_random_workload(eng, "MT-" + std::to_string(t), 100 + t, 3000);
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    for (uint64_t t = 0; t < 4; ++t) {
        CAPTURE(t);
        CHECK(eng.check_invariants("MT-" + std::to_string(t)).empty());
    }

    // Every trade ID was issued exactly once: the next one continues the sequence without gaps
    eng.submit_order(make_order("S", "MT-END", OrderType::LIMIT, Side::SELL, 100.0, 1.0));
    auto trades = eng.submit_order(make_order("B", "MT-END", OrderType::MARKET, Side::BUY, 0.0, 1.0));
    REQUIRE(trades.size() == 1U);
    size_t expected = 0;
    for (uint64_t t = 0; t < 4; ++t) {
        MatchingEngine solo;
        expected += run_random_workload(solo, "MT-" + std::to_string(t), 100 + t, 3000).trades;
    }
    CHECK(trades[0].trade_id == std::to_string(expected + 1));
}

} // TEST_SUITE("engine")
