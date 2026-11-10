// Determinism across runs, compilers and operating systems (P3.17, validation gate V1). Each scenario runs
// what `bs_sim run` runs (run_simulation) on a committed one-minute slice of real data, and its output hash
// (SHA-256 over the CSV outputs) must equal the pinned value. CI runs this on Windows (MSVC) and Linux
// (GCC): both have to produce the same bytes.
//
// The scenarios use every source of randomness and floating point: random latency (lognormal, normal),
// the random queue models (prob_power, prob_log) and pro_rata, a window with warm-up, both tie-break
// orders, and both reference strategies.
//
// When a change is MEANT to alter results, update the pinned hashes (the failure message prints the new
// ones) and say why in the commit. An unexplained change on one platform only is a determinism bug.
//
// Fixture: tests/sim/data/btcusdt_2026-11-05_0200_1m.bse, made with
//   bs_sim slice data/packed/binance_spot_btc_eth_1105_1107/BTCUSDT.bse
//       --from 2026-11-05T02:00:00Z --to 2026-11-05T02:01:00Z --out tests/sim/data/btcusdt_2026-11-05_0200_1m.bse
#include <doctest.h>
#include "sim_helpers.hpp"

#include "config.hpp"
#include "reconstruct.hpp"
#include "run.hpp"
#include "sha256.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#ifndef BLOCKSPRINT_TEST_DATA_DIR
#define BLOCKSPRINT_TEST_DATA_DIR "tests/sim/data"
#endif

using namespace simtest;
using nlohmann::json;

namespace {

const std::string kFixture = std::string(BLOCKSPRINT_TEST_DATA_DIR) + "/btcusdt_2026-11-05_0200_1m.bse";
const std::string kFixtureSha256 = "2e71bcf3751ce6aac9f1c3f2e345f74eed75de3c97805cde6e4795262ea2c406";

struct Scenario {
    const char* name;
    json config;
    const char* output_hash;
    uint64_t min_fills;  // so a scenario can't pass by doing nothing
};

json lognormal(double median_us, double sigma, double min_us) {
    return {{"model", "lognormal"}, {"median_us", median_us}, {"sigma", sigma}, {"min_us", min_us}};
}

std::vector<Scenario> scenarios() {
    return {
        {"quoter, random latency, prob_power:2",
         {{"latency", {{"feed", lognormal(2000, 0.5, 300)},
                       {"entry", {{"model", "normal"}, {"mean_us", 3000}, {"sd_us", 1000}, {"min_us", 500}}},
                       {"ack", lognormal(2500, 0.4, 400)}}},
          {"queue_model", {{"model", "prob_power"}, {"n", 2}}},
          {"seed", 7},
          {"strategy", {{"name", "passive_quoter"},
                        {"params", {{"size", "0.001"}, {"refresh_ms", 100}, {"max_position", "0.005"}}}}},
          {"output", {{"sample_ms", 250}}}},
         "e4983a252e78434fc0614d479c6e5ef46b05d75758e20f4ab3822b5f380c7728", 1},
        {"TWAP seller, home PC latency, prob_log, warm-up, orders first",
         {{"latency_preset", "binance_spot_home_pc"},
          {"queue_model", {{"model", "prob_log"}}},
          {"seed", 11},
          {"tie_break", "orders_first"},
          {"window", {{"warmup_s", 2}}},
          {"strategy", {{"name", "twap_taker"},
                        {"params", {{"side", "sell"}, {"quantity", "0.05"}, {"slices", 12}, {"duration_ms", 50000}}}}},
          {"output", {{"sample_ms", 500}}}},
         "4a71b6e682c666e2223c7e2da9c4a8949dacee2fe2a21587ef7dce8e641f0614", 12},
        {"quoter, Tokyo latency, pro_rata, a window inside the file",
         {{"latency_preset", "binance_spot_tokyo_vps"},
          {"queue_model", {{"model", "pro_rata"}}},
          {"seed", 3},
          {"window", {{"from", "2026-11-05T02:00:10Z"}, {"to", "2026-11-05T02:00:50Z"}, {"warmup_s", 5}}},
          {"strategy", {{"name", "passive_quoter"},
                        {"params", {{"size", "0.002"}, {"refresh_ms", 50}, {"post_only", false},
                                    {"max_position", "0.01"}}}}},
          {"output", {{"sample_ms", 100}}}},
         "5df4e779a1f5b4e649c175c361cc008ee02cbffa6d4b38d98977282aeff27351", 1},
    };
}

RunOutcome run(const json& scenario_config, const std::string& out_dir) {
    json cfg = scenario_config;
    cfg["dataset"] = kFixture;
    cfg["output"]["dir"] = out_dir;
    return run_simulation(resolve_run_config(cfg));
}

} // namespace

TEST_SUITE("determinism") {

TEST_CASE("slice: the book at the start as one snapshot, then the records unchanged; refused in a gap") {
    Stream st;
    st.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    st.depth(10, {{BID, 10001, 3}, {ASK, 10010, 0}, {ASK, 10012, 4}});
    st.trade(15, AGGR_BUY, 10012, 1);
    st.depth(20, {{BID, 10000, 0}});
    st.r.push_back(Stream::rec(30 * static_cast<int64_t>(kMs), bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH));
    st.snapshot(40, {{BID, 9990, 1}, {ASK, 9995, 1}});
    TempFile f;
    st.write(f);
    Replayer rp(f.str());

    const auto out = slice(rp, 12 * static_cast<int64_t>(kMs), 45 * static_cast<int64_t>(kMs));
    // SNAP_BEGIN, 3 levels (bids 100.01, 100.00; ask 100.12), SNAP_END; then trade, depth, gap, snapshot (4)
    REQUIRE(out.size() == 5 + 1 + 1 + 1 + 4);
    CHECK(out[0].kind == bse::SNAP_BEGIN);
    CHECK(out[0].ts_local == 12 * static_cast<int64_t>(kMs));
    CHECK(out[0].flags == (bse::FROM_SNAPSHOT | bse::SYNTHETIC_SNAPSHOT));
    CHECK((out[1].side == BID && out[1].px == 10001 && out[1].qty == 3));
    CHECK((out[2].side == BID && out[2].px == 10000 && out[2].qty == 5));
    CHECK((out[3].side == ASK && out[3].px == 10012 && out[3].qty == 4));
    CHECK(out[4].flags == (bse::FROM_SNAPSHOT | bse::SYNTHETIC_SNAPSHOT | bse::LAST_IN_BATCH));
    CHECK(out[5].kind == bse::TRADE);
    CHECK(out[7].kind == bse::GAP);
    CHECK(out.back().ts_local == 40 * static_cast<int64_t>(kMs));

    // The sliced file rebuilds the same book as the original from the start on
    TempFile g;
    bse::write_file(g.str(), rp.header(), out);
    Replayer a(f.str()), b(g.str());
    Reconstructor ra, rb;
    Batch batch;
    while (a.next(batch)) ra.apply(batch);
    while (b.next(batch)) rb.apply(batch);
    std::vector<Level> la, lb;
    for (uint8_t side = 0; side < 2; ++side) {
        ra.book().top(side, 100, la);
        rb.book().top(side, 100, lb);
        CHECK(la == lb);
    }

    // In the gap the next batch is the resync snapshot: the slice starts with it
    CHECK(slice(rp, 35 * static_cast<int64_t>(kMs), 45 * static_cast<int64_t>(kMs)).size() == 4);
    // ... but a depth event first can't be applied to an unknown book
    Stream bad;
    bad.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    bad.r.push_back(Stream::rec(10 * static_cast<int64_t>(kMs), bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH));
    bad.depth(15, {{BID, 10000, 4}});
    bad.snapshot(20, {{BID, 10000, 4}, {ASK, 10010, 2}});
    TempFile fb;
    bad.write(fb);
    Replayer rb_(fb.str());
    CHECK_THROWS_WITH_AS(slice(rb_, 12 * static_cast<int64_t>(kMs), 30 * static_cast<int64_t>(kMs)),
                         doctest::Contains("isn't valid at the start"), std::invalid_argument);
    CHECK_THROWS_AS(slice(rp, 12, 12), std::invalid_argument);
    // Starting at the data's own snapshot (here the start of the file): copied as it is
    const auto head = slice(rp, 0, 12 * static_cast<int64_t>(kMs));
    CHECK(head.size() == 4 + 3);  // the snapshot (begin, 2 levels, end) and the depth event at 10 (3 levels)
    CHECK(head[0].flags == bse::FROM_SNAPSHOT);

}

TEST_CASE("the fixture is the committed slice") {
    CHECK(Sha256::of_file(kFixture) == kFixtureSha256);
}

TEST_CASE("each scenario's output hash is the pinned one (same on every compiler and OS)") {
    std::string report;
    for (const auto& s : scenarios()) {
        CAPTURE(s.name);
        TempPath dir("");
        const RunOutcome r = run(s.config, dir.str());
        CHECK(r.account.fills >= s.min_fills);
        report += std::string("\n  ") + s.name + ": " + r.output_hash + " (" + std::to_string(r.account.fills) +
                  " fills)";
        CHECK_MESSAGE(r.output_hash == s.output_hash, "output hash changed: " << r.output_hash);
    }
    MESSAGE("output hashes:" << report);
}

TEST_CASE("a different seed gives a different hash: the randomness reaches the results") {
    const Scenario s = scenarios().front();
    json other = s.config;
    other["seed"] = 8;
    TempPath dir("");
    CHECK(run(other, dir.str()).output_hash != s.output_hash);
}

TEST_CASE("the same run twice in one process gives the same hash") {
    const Scenario s = scenarios().front();
    TempPath a(""), b("");
    CHECK(run(s.config, a.str()).output_hash == run(s.config, b.str()).output_hash);
}

} // TEST_SUITE("determinism")
