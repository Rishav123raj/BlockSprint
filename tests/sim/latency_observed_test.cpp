// Latency models (P3.8) and the observed book (P3.9).
#include <doctest.h>
#include "event_queue.hpp"
#include "latency_models.hpp"
#include "observed_book.hpp"
#include "reconstruct.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace sim;
namespace fs = std::filesystem;

namespace {

struct TempPath {
    fs::path path;
    explicit TempPath(const std::string& ext) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("bs_sim_lat_" + std::to_string(rd()) + std::to_string(rd()) + ext);
    }
    ~TempPath() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

struct Moments {
    double mean = 0, sd = 0, min = 1e300, max = -1e300;
};

Moments moments(const LatencyModel& m, Rng& rng, int n) {
    Moments r;
    double sum = 0, sum2 = 0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(m.sample_ns(rng));
        sum += x;
        sum2 += x * x;
        r.min = std::min(r.min, x);
        r.max = std::max(r.max, x);
    }
    r.mean = sum / n;
    r.sd = std::sqrt(sum2 / n - r.mean * r.mean);
    return r;
}

} // namespace

TEST_SUITE("latency") {

TEST_CASE("stream_rng: independent streams per name, same values as a reference implementation") {
    // Expected values from an independent Python implementation (FNV-1a of the name, SplitMix64, xoshiro256**)
    Rng feed = stream_rng(42, "feed/S1");
    CHECK(feed.next() == 0x77da7dca6b0ed868ULL);
    CHECK(feed.next() == 0xe544bd1125102531ULL);
    Rng entry = stream_rng(42, "entry/S1");
    CHECK(entry.next() == 0xef741baedd421910ULL);
    CHECK(entry.next() == 0xd54f3d8e86148b0dULL);
    Rng again = stream_rng(42, "feed/S1");
    CHECK(again.next() == 0x77da7dca6b0ed868ULL);
    CHECK(stream_rng(43, "feed/S1").next() != 0x77da7dca6b0ed868ULL);
}

TEST_CASE("constant: exact, and clamped to min/max") {
    Rng rng(1);
    CHECK(ConstantLatency(2500.0).sample_ns(rng) == 2500);
    CHECK(ConstantLatency(2500.4).sample_ns(rng) == 2500);
    CHECK(ConstantLatency(2500.5).sample_ns(rng) == 2501);
    CHECK(ConstantLatency(2500.0, 0.0, 1000.0).sample_ns(rng) == 1000);
    CHECK(ConstantLatency(500.0, 1000.0).sample_ns(rng) == 1000);
    CHECK_THROWS_AS(ConstantLatency(-1.0), std::invalid_argument);
    CHECK_THROWS_AS(ConstantLatency(1.0, 5.0, 2.0), std::invalid_argument);
}

TEST_CASE("normal: mean and sd as configured; never below the minimum") {
    Rng rng(2);
    auto m = moments(NormalLatency(3e6, 5e5), rng, 200000);
    CHECK(m.mean == doctest::Approx(3e6).epsilon(0.005));
    CHECK(m.sd == doctest::Approx(5e5).epsilon(0.01));
    auto clamped = moments(NormalLatency(1000.0, 2000.0, 100.0), rng, 20000);
    CHECK(clamped.min == 100.0);
    CHECK_THROWS_AS(NormalLatency(1.0, -1.0), std::invalid_argument);
}

TEST_CASE("lognormal: the median is as configured; sigma 0 is constant") {
    Rng rng(3);
    LogNormalLatency model(3e6, 0.4);
    std::vector<double> xs;
    for (int i = 0; i < 100001; ++i) {
        xs.push_back(static_cast<double>(model.sample_ns(rng)));
    }
    std::nth_element(xs.begin(), xs.begin() + 50000, xs.end());
    CHECK(xs[50000] == doctest::Approx(3e6).epsilon(0.01));
    CHECK(LogNormalLatency(2e6, 0.0).sample_ns(rng) == 2000000);
    auto m = moments(LogNormalLatency(3e6, 1.0, 0.0, 1e7), rng, 20000);
    CHECK(m.max == 1e7);
    CHECK_THROWS_AS(LogNormalLatency(0.0, 0.4), std::invalid_argument);
}

TEST_CASE("empirical: samples stay within the observed range and follow its distribution") {
    Rng rng(4);
    EmpiricalLatency model({1000, 2000, 3000, 4000, 5000});
    auto m = moments(model, rng, 100000);
    CHECK(m.min >= 1000.0);
    CHECK(m.max <= 5000.0);
    CHECK(m.mean == doctest::Approx(3000.0).epsilon(0.01));
    CHECK(model.distribution().quantile(0.5) == 3000.0);
    CHECK(model.distribution().quantile(0.125) == 1500.0);
}

TEST_CASE("latency files: units, comments, and bad input") {
    TempPath f(".txt");
    {
        std::ofstream out(f.str());
        out << "# feed latency, ms\n1.5\n\n  2\n0.25 \n";
    }
    auto values = read_latency_file(f.str(), "ms");
    CHECK(values == std::vector<double>{1.5e6, 2e6, 0.25e6});
    CHECK(read_latency_file(f.str(), "us") == std::vector<double>{1500.0, 2000.0, 250.0});
    CHECK_THROWS_AS(read_latency_file(f.str(), "s"), std::invalid_argument);
    {
        std::ofstream out(f.str());
        out << "1\n2x\n";
    }
    CHECK_THROWS_WITH_AS(read_latency_file(f.str(), "us"), doctest::Contains(":2:"), std::runtime_error);
    {
        std::ofstream out(f.str());
        out << "-3\n";
    }
    CHECK_THROWS_AS(read_latency_file(f.str(), "us"), std::runtime_error);
    {
        std::ofstream out(f.str());
        out << "# nothing\n";
    }
    CHECK_THROWS_WITH_AS(read_latency_file(f.str(), "us"), doctest::Contains("no values"), std::runtime_error);
    CHECK_THROWS_AS(read_latency_file(f.str() + ".missing", "us"), std::runtime_error);
}

TEST_CASE("models from config objects") {
    using nlohmann::json;
    Rng rng(5);
    CHECK(make_latency_model(json{{"model", "constant"}, {"us", 2000}})->sample_ns(rng) == 2000000);
    CHECK(make_latency_model(json{{"model", "constant"}, {"us", 2000}, {"max_us", 500}})->sample_ns(rng) == 500000);
    CHECK(make_latency_model(json{{"model", "normal"}, {"mean_us", 10}, {"sd_us", 0}})->sample_ns(rng) == 10000);
    CHECK(make_latency_model(json{{"model", "lognormal"}, {"median_us", 7}, {"sigma", 0}})->sample_ns(rng) == 7000);
    TempPath f(".txt");
    {
        std::ofstream out(f.str());
        out << "4\n4\n";
    }
    auto emp = make_latency_model(json{{"model", "empirical"}, {"file", f.str()}, {"unit", "ms"}});
    CHECK(emp->sample_ns(rng) == 4000000);
    CHECK(emp->describe().find("2 values") != std::string::npos);

    CHECK_THROWS_WITH_AS(make_latency_model(json{{"model", "uniform"}}), doctest::Contains("unknown"), std::invalid_argument);
    CHECK_THROWS_WITH_AS(make_latency_model(json{{"model", "constant"}}), doctest::Contains("\"us\""), std::invalid_argument);
    CHECK_THROWS_AS(make_latency_model(json{{"model", "constant"}, {"us", "2"}}), std::invalid_argument);
    CHECK_THROWS_AS(make_latency_model(json{{"us", 2}}), std::invalid_argument);
    CHECK_THROWS_AS(make_latency_model(json{{"model", "empirical"}}), std::invalid_argument);
    CHECK_THROWS_AS(make_latency_model(json{{"model", "constant"}, {"us", 5}, {"min_us", 9}, {"max_us", 1}}),
                    std::invalid_argument);
}

TEST_CASE("a channel never lets a message overtake an earlier one") {
    NormalLatency model(5000.0, 4000.0);  // samples vary far more than the gap between sends
    LatencyChannel channel(model, stream_rng(7, "entry/S1"));
    uint64_t prev = 0;
    bool ordered = true, after_send = true;
    for (uint64_t send = 1000; send < 1000 + 100 * 10000; send += 100) {
        const uint64_t t = channel.arrival(send);
        ordered &= t >= prev;
        after_send &= t >= send;
        prev = t;
    }
    CHECK(ordered);
    CHECK(after_send);
    CHECK(channel.held_back() > 1000);  // it did have to hold messages back
    CHECK(channel.last_arrival() == prev);
}

TEST_CASE("regime latency: the curve, the message-rate window, the scaled delay") {
    LatencyRegimeConfig c;
    c.curve = {{100.0, 1.0}, {200.0, 3.0}};
    LatencyRegime regime(c);
    CHECK(regime.at_rate(0.0) == 1.0);    // flat below the first point
    CHECK(regime.at_rate(150.0) == 2.0);  // linear between points
    CHECK(regime.at_rate(1e6) == 3.0);    // flat beyond the last

    const uint64_t s = 1000000000ULL;
    regime.record(0, 5);
    regime.record(s / 2, 5);
    CHECK(regime.rate(9 * s / 10) == 10.0);
    CHECK(regime.rate(s) == 5.0);  // the records at 0 leave the 1 s window
    CHECK(regime.rate(3 * s / 2) == 0.0);
    CHECK(regime.samples() == 0);
    CHECK(regime.multiplier(3 * s / 2) == 1.0);
    CHECK(regime.samples() == 1);

    ConstantLatency model(1000.0);
    LatencyChannel channel(model, stream_rng(1, "entry/S1"));
    CHECK(channel.arrival(0, 2.5) == 2500);
    CHECK(channel.arrival(10000) == 11000);

    auto bad = [](std::vector<std::pair<double, double>> curve, uint64_t window_ns = 1000000000ULL) {
        LatencyRegimeConfig b;
        b.curve = std::move(curve);
        b.window_ns = window_ns;
        CHECK_THROWS_AS(LatencyRegime{b}, std::invalid_argument);
    };
    bad({{100.0, 1.0}, {100.0, 2.0}});  // rates must increase
    bad({{100.0, 0.0}});                // multiplier must be > 0
    bad({{-1.0, 1.0}});
    bad({{100.0, 1.0}}, 0);
}

TEST_CASE("the same seed gives the same delays (pinned against a run on each compiler)") {
    LogNormalLatency model(3e6, 0.4);
    LatencyChannel a(model, stream_rng(42, "feed/S1"));
    LatencyChannel b(model, stream_rng(42, "feed/S1"));
    std::vector<uint64_t> xs;
    for (uint64_t t = 0; t < 5; ++t) {
        const uint64_t x = a.arrival(t * 10000000);
        CHECK(x == b.arrival(t * 10000000));
        xs.push_back(x);
    }
    // Pinned on MSVC and checked on GCC: any change to the RNG, the normal sampler or det_exp shows up here
    CHECK(xs == std::vector<uint64_t>{2908911, 14401293, 23502274, 33422940, 43474585});
}

} // TEST_SUITE

TEST_SUITE("observed_book") {

namespace {

bse::Record rec(int64_t ts, uint8_t kind, uint8_t side = 0, int64_t px = 0, int64_t qty = 0, uint16_t flags = 0) {
    return bse::Record{ts, ts - 3, px, qty, 1, kind, side, flags};
}

std::vector<bse::Record> snapshot(int64_t ts, int64_t bid_px, int64_t ask_px) {
    return {rec(ts, bse::SNAP_BEGIN, 0, 0, 0, bse::FROM_SNAPSHOT), rec(ts, bse::SNAP_LEVEL, 0, bid_px, 5),
            rec(ts, bse::SNAP_LEVEL, 0, bid_px - 1, 3), rec(ts, bse::SNAP_LEVEL, 1, ask_px, 4),
            rec(ts, bse::SNAP_END, 0, 0, 0, bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH)};
}

// t=10 snapshot (bid 100 x5, 99 x3; ask 101 x4); 20 depth; 25 trade; 30 depth; 40 gap; 45 depth; 50 snapshot
std::vector<bse::Record> stream() {
    std::vector<bse::Record> r = snapshot(10, 100, 101);
    r.push_back(rec(20, bse::DELTA, 0, 100, 7));
    r.push_back(rec(20, bse::DELTA, 1, 102, 2, bse::LAST_IN_BATCH));
    r.push_back(rec(25, bse::TRADE, 1, 100, 1));
    r.push_back(rec(30, bse::DELTA, 1, 101, 0, bse::LAST_IN_BATCH));
    r.push_back(rec(40, bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH));
    r.push_back(rec(45, bse::DELTA, 0, 98, 1, bse::LAST_IN_BATCH));
    for (const auto& s : snapshot(50, 96, 97)) {
        r.push_back(s);
    }
    return r;
}

constexpr int64_t kTick = 1000000;  // 0.01
constexpr int64_t kLot = 100000;    // 0.001

bse::Header header() {
    bse::Header h;
    h.symbol = "TESTUSDT";
    h.tick_size = kTick;
    h.lot_size = kLot;
    h.price_decimals = 2;
    h.qty_decimals = 3;
    return h;
}

} // namespace

TEST_CASE("after the same batches the observed book equals the reconstructed book") {
    TempPath f(".bse");
    bse::write_file(f.str(), header(), stream());
    Replayer rp(f.str());
    Reconstructor truth;
    ObservedBook seen(kTick, kLot);
    Batch b{};
    std::vector<Level> want;
    std::vector<BookLevel> got;
    int compared = 0;
    while (rp.next(b)) {
        truth.apply(b);
        seen.apply(b, static_cast<uint64_t>(b.ts_local()) + 5);
        CHECK(seen.valid() == truth.valid());
        for (uint8_t side = 0; side < 2; ++side) {
            truth.book().top(side, 50, want);
            seen.top(side == 0 ? Side::BUY : Side::SELL, 50, got);
            REQUIRE(got.size() == want.size());
            for (size_t i = 0; i < got.size(); ++i) {
                CHECK(got[i] == BookLevel{want[i].px * kTick, want[i].qty * kLot});
            }
        }
        ++compared;
    }
    CHECK(compared == 7);
    CHECK(seen.batches() == 7);
    CHECK(seen.data_time() == 50);
    CHECK(seen.seen_at() == 55);
}

TEST_CASE("reading the view: best levels, quantities, the last trade, gaps") {
    TempPath f(".bse");
    bse::write_file(f.str(), header(), stream());
    Replayer rp(f.str());
    ObservedBook book(kTick, kLot);
    Batch b{};
    CHECK_FALSE(book.valid());
    CHECK_FALSE(book.best_bid());

    rp.next(b);  // snapshot
    book.apply(b, 10);
    REQUIRE(book.best_bid());
    CHECK(*book.best_bid() == BookLevel{100 * kTick, 5 * kLot});
    CHECK(*book.best_ask() == BookLevel{101 * kTick, 4 * kLot});
    CHECK(book.qty(Side::BUY, 99 * kTick) == 3 * kLot);
    CHECK(book.qty(Side::BUY, 99 * kTick + 1) == 0);  // off the grid
    CHECK(book.qty(Side::SELL, 99 * kTick) == 0);
    CHECK(book.levels(Side::BUY) == 2);

    rp.next(b);  // depth
    book.apply(b, 20);
    rp.next(b);  // trade
    book.apply(b, 25);
    REQUIRE(book.last_trade());
    CHECK(book.last_trade()->price == 100 * kTick);
    CHECK(book.last_trade()->qty == 1 * kLot);
    CHECK(book.last_trade()->aggressor == Side::SELL);
    CHECK(book.last_trade()->ts_local == 25);
    CHECK(book.last_trade()->ts_exch == 22);
    CHECK(*book.best_bid() == BookLevel{100 * kTick, 7 * kLot});  // a trade doesn't change the book

    rp.next(b);  // depth: ask 101 gone
    book.apply(b, 30);
    CHECK(*book.best_ask() == BookLevel{102 * kTick, 2 * kLot});
    rp.next(b);  // gap
    book.apply(b, 40);
    CHECK_FALSE(book.valid());
    CHECK_FALSE(book.best_bid());
    CHECK(book.last_trade());  // the tape survives a gap
}

TEST_CASE("feed latency: at any sim time the strategy sees the market as of an earlier time") {
    // Batches at 10 (snapshot), 20, 25 (trade), 30 delivered through a constant 7 ns feed. At sim time 26
    // the strategy has seen only the snapshot (20 arrives at 27): bid 100 x 5, not the newer 7.
    TempPath f(".bse");
    bse::write_file(f.str(), header(), stream());
    Replayer rp(f.str());
    ConstantLatency feed_model(7.0);
    LatencyChannel feed(feed_model, stream_rng(1, "feed/S1"));
    EventQueue<Batch> q;
    Batch b{};
    while (rp.next(b)) {
        q.push(feed.arrival(static_cast<uint64_t>(b.ts_local())), EventClass::STRATEGY_MD, b);
    }
    ObservedBook seen(kTick, kLot);
    Reconstructor truth;
    rp.rewind();
    while (!q.empty() && q.top().time <= 26) {
        auto e = q.pop();
        seen.apply(e.payload, e.time);
    }
    while (rp.next(b) && b.ts_local() <= 26) {
        truth.apply(b);
    }
    CHECK(seen.data_time() == 10);
    CHECK(seen.seen_at() == 17);
    CHECK(*seen.best_bid() == BookLevel{100 * kTick, 5 * kLot});
    CHECK(truth.book().best_bid()->qty == 7);  // the market already moved
    // Delivered in data order, each 7 ns late
    uint64_t prev = 17;
    while (!q.empty()) {
        auto e = q.pop();
        CHECK(e.time == static_cast<uint64_t>(e.payload.ts_local()) + 7);
        CHECK(e.time >= prev);
        prev = e.time;
    }
}

} // TEST_SUITE
