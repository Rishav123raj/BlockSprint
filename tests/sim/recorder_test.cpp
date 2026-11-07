// Result recorder (P3.13) and SHA-256: file contents for golden (e), output hashes, run metadata.
#include <doctest.h>
#include "recorder.hpp"
#include "sha256.hpp"
#include "sim_helpers.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace simtest;
using nlohmann::json;

namespace {

std::vector<std::string> lines_of(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

std::string text_of(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// Golden (e) with 5 ms latency: the stale ask is picked off at 22, the cancel (sent 25) is refused at 30
void golden_e(Script& s) {
    auto st = std::make_shared<std::pair<OrderId, bool>>();
    s.book = [st](Script&, Context& ctx, const ObservedBook& b) {
        if (!b.valid()) return;
        if (st->first.empty()) {
            st->first = ctx.send(Side::SELL, OrderType::LIMIT, b.best_ask()->price, kLot);
        } else if (!st->second && ctx.open_orders().count(st->first) &&
                   b.best_ask()->price > ctx.open_orders().at(st->first).price) {
            ctx.cancel(st->first);
            st->second = true;
        }
    };
}

// 1000 ms of moving bids and trades, and a strategy that keeps a bid at the best bid
void busy_stream(const TempFile& f) {
    Stream s;
    s.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 5}});
    Rng data_rng(5);
    int64_t bid = 10000;
    for (int64_t ms = 1; ms <= 1000; ++ms) {
        const int64_t move = static_cast<int64_t>(data_rng.next() % 3) - 1;
        if (move != 0) {
            const int64_t nb = bid + move;
            s.depth(ms, {{BID, bid, 0}, {BID, nb, 1 + static_cast<int64_t>(data_rng.next() % 9)}, {ASK, bid + 1 + move, 0},
                         {ASK, nb + 10, 5}});
            bid = nb;
        } else {
            s.trade(ms, data_rng.next() % 2 ? AGGR_BUY : 1, bid, 1 + static_cast<int64_t>(data_rng.next() % 4));
        }
    }
    s.write(f);
}

void bid_keeper(Script& s) {
    auto live = std::make_shared<OrderId>();
    s.book = [live](Script&, Context& ctx, const ObservedBook& b) {
        if (!b.valid() || !b.best_bid()) return;
        auto it = ctx.open_orders().find(*live);
        if (it == ctx.open_orders().end()) {
            *live = ctx.send(Side::BUY, OrderType::LIMIT, b.best_bid()->price, kLot);
        } else if (it->second.status == OrderStatus::NEW && it->second.price != b.best_bid()->price) {
            ctx.cancel(*live);
        }
    };
}

} // namespace

TEST_SUITE("recorder") {

TEST_CASE("sha256 matches the FIPS 180-4 vectors, in one piece or streamed") {
    CHECK(Sha256::of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(Sha256::of(two_blocks) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 h;
    const std::string a1000(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(a1000);
    CHECK(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Sha256 parts;
    for (char c : two_blocks) parts.update(&c, 1);
    CHECK(parts.hex() == Sha256::of(two_blocks));
}

TEST_CASE("golden (e) recorded: requests, reports, refused cancel, samples, final position, metadata") {
    TempFile f;
    Stream data;
    data.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    data.depth(20, {{ASK, 10010, 0}, {ASK, 10020, 3}});
    data.trade(22, AGGR_BUY, 10020, 1);
    data.depth(40, {{BID, 10000, 6}});
    data.write(f);
    TempPath out("");

    Simulator sim(f.str(), SimulatorOptions{});
    Script s;
    golden_e(s);
    sim.add_strategy(s, session("S1", 5));
    json meta;
    {
        ResultRecorder rec(sim, out.str(), 10 * kMs);
        sim.run();
        meta = rec.finish(json{{"git_sha", "abc123"}, {"wall_seconds", 0.5}});
    }

    const auto orders = lines_of(out.path / "orders.csv");
    REQUIRE(orders.size() == 3);
    CHECK(orders[0] == "sent_ns,arrive_ns,owner,request,order_id,side,order_type,price,quantity,new_price,stp");
    CHECK(starts_with(orders[1], "5000000,10000000,S1,new,S1-1," + sideToString(Side::SELL) + "," +
                                     orderTypeToString(OrderType::LIMIT) + ",100.1,0.001,,"));
    CHECK(orders[2] == "25000000,30000000,S1,cancel,S1-1,,,,,,");

    const auto reports = lines_of(out.path / "exec_reports.csv");
    REQUIRE(reports.size() == 3);
    CHECK(starts_with(reports[1], "10000000,15000000,S1,"));
    CHECK(reports[1].find(",S1-1,ack,,") != std::string::npos);
    CHECK(starts_with(reports[2], "22000000,27000000,S1,"));
    CHECK(reports[2].find(",S1-1,fill,,") != std::string::npos);
    CHECK(reports[2].find(",100.1,0.001,100.1,0.001,0.001,0.0," + liquidityToString(Liquidity::MAKER) + ",") !=
          std::string::npos);

    const auto rejects = lines_of(out.path / "request_rejects.csv");
    REQUIRE(rejects.size() == 2);
    CHECK(rejects[1] == "30000000,35000000,S1,cancel,S1-1,,\"Order is not open\"");

    // Samples at 0, 10, 20, 30, 40 (the run ends just after the last data, 40 ms + 1 ns)
    const auto bbo = lines_of(out.path / "bbo_samples.csv");
    REQUIRE(bbo.size() == 6);
    CHECK(starts_with(bbo[1], "0,S1,"));
    // At 20, before the data at 20: the book holds our 0.001 at the ask; the strategy saw the snapshot at 5
    CHECK(bbo[3] == "20000000,S1,100.0,0.005,100.1,0.002,100.0,0.005,100.1,0.003,100.0,0.005,100.1,0.002,0");
    const auto pos = lines_of(out.path / "positions.csv");
    REQUIRE(pos.size() == 7);  // 5 samples and the end
    CHECK(starts_with(pos[3], "20000000,S1,0.0,0.0,0.0,100.05,0.0,0,0,0,1,"));
    // The end: short 0.001 sold at 100.10, marked at the data's mid (100.00 / 100.20)
    CHECK(pos[6] == "40000001,S1,-0.001,0.1001,0.0,100.1,0.0,1,1,0,0,0.001,0.1001");

    // Metadata: what the caller added, the outputs and their hash
    const json written = json::parse(text_of(out.path / "run_meta.json"));
    CHECK(written == meta);
    CHECK(meta["git_sha"] == "abc123");
    CHECK(meta["format"] == "bs_sim.run/1");
    CHECK(meta["window"]["end_ns"] == 40 * kMs + 1);
    CHECK(meta["outputs"]["orders.csv"]["rows"] == 2);
    CHECK(meta["outputs"]["exec_reports.csv"]["rows"] == 2);
    CHECK(meta["outputs"]["request_rejects.csv"]["rows"] == 1);
    CHECK(meta["outputs"]["orders.csv"]["sha256"] == Sha256::of_file((out.path / "orders.csv").string()));
    CHECK(meta["sessions"][0]["owner"] == "S1");
    CHECK(meta["sessions"][0]["strategy"] == "script");
    CHECK(meta["sessions"][0]["exchange_account"]["position"] == "-0.001");
    CHECK(meta["sessions"][0]["stats"]["request_rejects"] == 1);
    CHECK(meta["stats"]["events"]["order_arrive"] == 2);
    CHECK(text_of(out.path / "output_hash") == meta["output_hash"].get<std::string>() + "\n");
}

TEST_CASE("output hashes: the same seed gives the same files, another seed other files") {
    TempFile f;
    busy_stream(f);
    auto run = [&](uint64_t seed, const TempPath& out) {
        SimulatorOptions opt;
        opt.seed = seed;
        opt.queue_model = "prob_log";
        Simulator sim(f.str(), opt);
        Script s;
        bid_keeper(s);
        SessionConfig c;
        c.owner = "S1";
        c.feed = std::make_shared<LogNormalLatency>(1e6, 0.5);
        c.entry = std::make_shared<LogNormalLatency>(2e6, 0.5);
        c.ack = std::make_shared<LogNormalLatency>(1e6, 0.5);
        c.fees = FeeSchedule{100000, 100000};
        sim.add_strategy(s, c);
        ResultRecorder rec(sim, out.str(), 50 * kMs);
        sim.run();
        const json meta = rec.finish();
        return std::make_pair(rec.output_hash(), meta);
    };
    TempPath a(""), b(""), c("");
    const auto ra = run(7, a);
    const auto rb = run(7, b);
    const auto rc = run(8, c);
    CHECK(ra.second["outputs"]["exec_reports.csv"]["rows"].get<uint64_t>() > 50);
    CHECK(ra.first == rb.first);
    CHECK(ra.second == rb.second);
    for (const char* name : {"orders.csv", "exec_reports.csv", "request_rejects.csv", "bbo_samples.csv", "positions.csv"}) {
        CHECK(text_of(a.path / name) == text_of(b.path / name));
    }
    CHECK(ra.first != rc.first);
}

TEST_CASE("run_inputs: the resolved config and its hash, the data file and the manifest's hash") {
    TempPath dir("");
    fs::create_directories(dir.path);
    Stream data;
    data.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    bse::write_file((dir.path / "TESTUSDT.bse").string(), test_header(), data.r);
    std::ofstream(dir.path / "manifest.json")
        << json{{"files", json::array({{{"symbol", "TESTUSDT"}, {"file", "TESTUSDT.bse"}, {"sha256", "ff"}}})}}.dump();
    const RunConfig rc = resolve_run_config(
        json{{"dataset", dir.str()}, {"latency_preset", "zero"}, {"strategy", {{"name", "x"}}}});
    const json in = run_inputs(rc);
    CHECK(in["config"] == rc.resolved);
    CHECK(in["config_sha256"] == Sha256::of(rc.resolved.dump()));
    CHECK(in["data"]["symbol"] == "TESTUSDT");
    CHECK(in["data"]["sha256"] == "ff");
    CHECK(in["data"]["manifest_sha256"] == Sha256::of_file((dir.path / "manifest.json").string()));
}

TEST_CASE("no sampling when the interval is 0; finish only once") {
    TempFile f;
    Stream data;
    data.snapshot(0, {{BID, 10000, 5}, {ASK, 10010, 2}});
    data.write(f);
    TempPath out("");
    Simulator sim(f.str(), SimulatorOptions{});
    Script s;
    sim.add_strategy(s, session("S1", 1));
    ResultRecorder rec(sim, out.str(), 0);
    sim.run();
    const json meta = rec.finish();
    CHECK(meta["outputs"]["bbo_samples.csv"]["rows"] == 0);
    CHECK(meta["outputs"]["positions.csv"]["rows"] == 1);  // the end
    CHECK_THROWS_AS(rec.finish(), std::logic_error);
}

} // TEST_SUITE("recorder")
