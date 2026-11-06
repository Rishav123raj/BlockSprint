// .bse reader (P3.4) and reconstruction mode (P3.5).
#include <doctest.h>
#include "reconstruct.hpp"
#include "replayer.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace sim;
namespace fs = std::filesystem;

namespace {

struct TempFile {
    fs::path path;
    TempFile() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("bs_sim_test_" + std::to_string(rd()) + std::to_string(rd()) + ".bse");
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

bse::Header test_header() {
    bse::Header h;
    h.symbol = "TESTUSDT";
    h.tick_size = 1000000;  // 0.01
    h.lot_size = 100000;    // 0.001
    h.price_decimals = 2;
    h.qty_decimals = 3;
    h.min_notional = 500000000;
    h.source_manifest_sha256[0] = 0xab;
    return h;
}

bse::Record rec(int64_t ts, uint8_t kind, uint8_t side = 0, int64_t px = 0, int64_t qty = 0, uint16_t flags = 0,
                uint32_t seq = 1) {
    return bse::Record{ts, ts, px, qty, seq, kind, side, flags};
}

// A snapshot at t=10: bids 100 x 5, 99 x 3; asks 101 x 4
std::vector<bse::Record> snapshot(int64_t ts) {
    return {rec(ts, bse::SNAP_BEGIN, 0, 0, 0, bse::FROM_SNAPSHOT), rec(ts, bse::SNAP_LEVEL, 0, 100, 5),
            rec(ts, bse::SNAP_LEVEL, 0, 99, 3), rec(ts, bse::SNAP_LEVEL, 1, 101, 4),
            rec(ts, bse::SNAP_END, 0, 0, 0, bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH)};
}

std::vector<bse::Record> sample_stream() {
    std::vector<bse::Record> r = snapshot(10);
    r.push_back(rec(20, bse::DELTA, 0, 100, 7));                       // bid 100: 5 -> 7
    r.push_back(rec(20, bse::DELTA, 1, 102, 2, bse::LAST_IN_BATCH));   // new ask 102
    r.push_back(rec(25, bse::TRADE, 1, 100, 1));                       // trades don't change the book
    r.push_back(rec(30, bse::DELTA, 1, 101, 0, bse::LAST_IN_BATCH));   // ask 101 removed
    r.push_back(rec(40, bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH));
    r.push_back(rec(45, bse::DELTA, 0, 98, 1, bse::LAST_IN_BATCH));    // during the gap: applied, book invalid
    for (const auto& s : snapshot(50)) {
        r.push_back(s);
    }
    return r;
}

} // namespace

TEST_SUITE("bse") {

TEST_CASE("header round trip and validation") {
    TempFile f;
    auto records = sample_stream();
    bse::write_file(f.str(), test_header(), records);
    Replayer rp(f.str());
    const auto& h = rp.header();
    CHECK(h.symbol == "TESTUSDT");
    CHECK(h.record_count == records.size());
    CHECK(h.first_ts_local == 10);
    CHECK(h.last_ts_local == 50);
    CHECK(h.tick_size == 1000000);
    CHECK(h.lot_size == 100000);
    CHECK(h.price_decimals == 2);
    CHECK(h.qty_decimals == 3);
    CHECK(h.min_notional == 500000000);
    CHECK(h.source_manifest_sha256[0] == 0xab);
    CHECK(rp.records()[5].px == 100);
    CHECK(rp.records()[5].qty == 7);
}

TEST_CASE("a corrupt file is rejected") {
    TempFile f;
    bse::write_file(f.str(), test_header(), sample_stream());
    auto patch = [&](std::streamoff offset, const std::string& bytes) {
        std::fstream io(f.str(), std::ios::in | std::ios::out | std::ios::binary);
        io.seekp(offset);
        io.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    SUBCASE("magic") {
        patch(0, "XXXXXXXX");
        CHECK_THROWS_WITH_AS(Replayer(f.str()), doctest::Contains("wrong magic"), std::runtime_error);
    }
    SUBCASE("version") {
        patch(8, std::string("\x02\0\0\0", 4));
        CHECK_THROWS_WITH_AS(Replayer(f.str()), doctest::Contains("unsupported"), std::runtime_error);
    }
    SUBCASE("reserved bytes") {
        patch(300, "x");
        CHECK_THROWS_WITH_AS(Replayer(f.str()), doctest::Contains("reserved"), std::runtime_error);
    }
    SUBCASE("truncated") {
        fs::resize_file(f.path, fs::file_size(f.path) - 1);
        CHECK_THROWS_WITH_AS(Replayer(f.str()), doctest::Contains("records need"), std::runtime_error);
    }
    SUBCASE("missing file") {
        CHECK_THROWS_AS(Replayer(f.str() + ".missing"), std::runtime_error);
    }
}

TEST_CASE("records are grouped into batches") {
    TempFile f;
    bse::write_file(f.str(), test_header(), sample_stream());
    Replayer rp(f.str());
    std::vector<std::pair<BatchKind, size_t>> got;
    Batch b{};
    while (rp.next(b)) {
        got.emplace_back(b.kind, b.size());
    }
    CHECK(got == std::vector<std::pair<BatchKind, size_t>>{
                     {BatchKind::SNAPSHOT, 5}, {BatchKind::DEPTH, 2}, {BatchKind::TRADE, 1}, {BatchKind::DEPTH, 1},
                     {BatchKind::GAP, 1}, {BatchKind::DEPTH, 1}, {BatchKind::SNAPSHOT, 5}});
    CHECK_FALSE(rp.next(b));
}

TEST_CASE("a cut-off depth event or snapshot is reported with its record index") {
    TempFile f;
    auto records = snapshot(10);
    records.push_back(rec(20, bse::DELTA, 0, 100, 7));  // no LAST_IN_BATCH
    records.push_back(rec(21, bse::TRADE, 1, 100, 1));
    bse::write_file(f.str(), test_header(), records);
    Replayer rp(f.str());
    Batch b{};
    CHECK(rp.next(b));
    CHECK_THROWS_WITH_AS(rp.next(b), doctest::Contains("record 5: depth event without a LAST_IN_BATCH"),
                         std::runtime_error);

    TempFile g;
    auto cut = snapshot(10);
    cut.pop_back();
    bse::write_file(g.str(), test_header(), cut);
    Replayer rp2(g.str());
    CHECK_THROWS_WITH_AS(rp2.next(b), doctest::Contains("snapshot without SNAP_END"), std::runtime_error);
}

TEST_CASE("an empty file has a header and no batches") {
    TempFile f;
    bse::write_file(f.str(), test_header(), {});
    Replayer rp(f.str());
    Batch b{};
    CHECK(rp.record_count() == 0);
    CHECK_FALSE(rp.next(b));
}

} // TEST_SUITE("bse")

TEST_SUITE("reconstruct") {

TEST_CASE("the book follows snapshots, deltas and gaps like the Python oracle") {
    TempFile f;
    bse::write_file(f.str(), test_header(), sample_stream());
    Replayer rp(f.str());
    Reconstructor r;
    Batch b{};

    rp.next(b);
    r.apply(b);
    CHECK(r.valid());
    CHECK(r.book().best_bid() == Level{100, 5});
    CHECK(r.book().best_ask() == Level{101, 4});

    rp.next(b);
    r.apply(b);
    CHECK(r.book().qty(0, 100) == 7);
    CHECK(r.book().qty(1, 102) == 2);

    rp.next(b);  // trade
    r.apply(b);
    CHECK(r.book().qty(0, 100) == 7);

    rp.next(b);
    r.apply(b);
    CHECK(r.book().best_ask() == Level{102, 2});

    rp.next(b);  // gap
    r.apply(b);
    CHECK_FALSE(r.valid());
    CHECK(r.book().levels(0) == 0);

    while (rp.next(b)) {
        r.apply(b);
    }
    CHECK(r.valid());
    CHECK(r.book().best_bid() == Level{100, 5});
    CHECK(r.book().levels(0) == 2);  // the snapshot replaced the level set during the gap
    const auto& s = r.stats();
    CHECK(s.book_batches == 6);       // 2 snapshots, 3 depth events, 1 gap
    CHECK(s.batches[static_cast<int>(BatchKind::TRADE)] == 1);
    CHECK(s.segments == 2);
    CHECK(s.crossed_batches == 0);
    CHECK(s.ts_backwards == 0);
}

TEST_CASE("a crossed book is counted") {
    TempFile f;
    auto records = snapshot(10);
    records.push_back(rec(20, bse::DELTA, 0, 101, 1, bse::LAST_IN_BATCH));  // bid at the ask: locked
    bse::write_file(f.str(), test_header(), records);
    Replayer rp(f.str());
    Reconstructor r;
    ReconstructStats s = reconstruct(rp, r, {}, nullptr);
    CHECK(s.crossed_batches == 1);
}

TEST_CASE("checkpoints dump the top levels per side, by batch count and by time") {
    TempFile f;
    bse::write_file(f.str(), test_header(), sample_stream());
    Replayer rp(f.str());
    Reconstructor r;
    ReconstructOptions opt;
    opt.top_n = 1;
    opt.every = 2;
    opt.at_ts = {26};
    std::ostringstream out;
    ReconstructStats s = reconstruct(rp, r, opt, &out);
    CHECK(s.checkpoints == 5);  // every 2 book batches (3 of 6), at t=26, final
    CHECK(out.str() ==
          "batch,ts_local,valid,side,level,px,qty\n"
          "2,20,1,0,0,100,7\n2,20,1,1,0,101,4\n"   // after the snapshot and the first depth event
          "2,26,1,0,0,100,7\n2,26,1,1,0,101,4\n"   // at t=26: same state (the trade didn't change it)
          "4,40,0,,,,\n"                           // after the gap: invalid
          "6,50,1,0,0,100,5\n6,50,1,1,0,101,4\n"   // after the resync snapshot
          "6,50,1,0,0,100,5\n6,50,1,1,0,101,4\n"); // final
}

} // TEST_SUITE("reconstruct")
