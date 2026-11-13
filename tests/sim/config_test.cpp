// Run config (P3.12): layering, presets, overrides, times, dataset resolution, errors that name the key.
#include <doctest.h>
#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

using namespace sim;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// A dataset directory: TESTUSDT.bse and OTHERUSDT.bse with a manifest
struct Dataset {
    fs::path dir;
    Dataset() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("bs_sim_cfg_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir);
        write("TESTUSDT");
        write("OTHERUSDT");
        json m{{"dataset", "test"},
               {"files", json::array({{{"symbol", "TESTUSDT"}, {"file", "TESTUSDT.bse"}, {"sha256", std::string(64, 'a')}},
                                      {{"symbol", "OTHERUSDT"}, {"file", "OTHERUSDT.bse"}, {"sha256", std::string(64, 'b')}}})}};
        std::ofstream(dir / "manifest.json") << m.dump(2);
    }
    ~Dataset() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& symbol) const {
        bse::Header h;
        h.symbol = symbol;
        h.tick_size = 1000000;
        h.lot_size = 100000;
        h.price_decimals = 2;
        h.qty_decimals = 3;
        const int64_t t = 1793836800000000000LL;
        bse::write_file((dir / (symbol + ".bse")).string(), h,
                        {bse::Record{t, t, 0, 0, 1, bse::SNAP_BEGIN, 0, bse::FROM_SNAPSHOT},
                         bse::Record{t, t, 10000, 5, 1, bse::SNAP_LEVEL, 0, bse::FROM_SNAPSHOT},
                         bse::Record{t, t, 0, 0, 1, bse::SNAP_END, 0, bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH}});
    }
    std::string str() const { return dir.string(); }
};

json minimal(const Dataset& d) {
    return json{{"dataset", d.str()},
                {"symbol", "TESTUSDT"},
                {"latency_preset", "zero"},
                {"strategy", {{"name", "passive_quoter"}}}};
}

std::string error_of(const json& cfg, const std::vector<std::string>& overrides = {}) {
    try {
        resolve_run_config(cfg, overrides);
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return "no error";
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_SUITE("config") {

TEST_CASE("a minimal config gets the defaults; the dataset manifest gives the file") {
    Dataset d;
    const RunConfig rc = resolve_run_config(minimal(d));
    CHECK(rc.symbol == "TESTUSDT");
    CHECK(fs::path(rc.bse_path) == d.dir / "TESTUSDT.bse");
    CHECK(rc.bse_sha256 == std::string(64, 'a'));
    CHECK(fs::path(rc.manifest_path) == d.dir / "manifest.json");
    CHECK(rc.sim.from_ns == 0);
    CHECK(rc.sim.to_ns == 0);
    CHECK(rc.sim.warmup_ns == 0);
    CHECK(rc.sim.seed == 0);
    CHECK(rc.sim.tie_break == TieBreak::DATA_FIRST);
    CHECK(rc.sim.queue_model == "cancel_from_back");
    CHECK(rc.fees.maker_rate == 100000);  // 0.1%
    CHECK(rc.fees.taker_rate == 100000);
    CHECK(rc.limits.orders_per_10s == 100);
    CHECK(rc.limits.orders_per_day == 200000);
    CHECK(rc.limits.max_open_orders == 200);
    CHECK(rc.limits.request_weight_per_min == 6000);
    CHECK_FALSE(rc.sim.latency_regime.enabled());
    CHECK_FALSE(rc.risk.any());
    CHECK_FALSE(rc.sim.permanent_impact.enabled());
    CHECK(rc.gap == GapPolicy::HALT);
    CHECK(rc.sim.latency_regime.window_ns == 1000000000ULL);
    CHECK(rc.strategy_name == "passive_quoter");
    CHECK(rc.owner == "S1");
    CHECK(rc.strategy_params == json::object());
    CHECK(rc.output_dir == "results/run");
    CHECK(rc.sample_ns == 1000000000ULL);
    Rng rng(1);
    CHECK(rc.feed->sample_ns(rng) == 0);
    const SessionConfig s = rc.session();
    CHECK(s.owner == "S1");
    CHECK(s.entry == rc.entry);
    CHECK(s.limits.max_open_orders == 200);
}

TEST_CASE("precedence: defaults < preset < file < overrides; a model object naming its model replaces the whole model") {
    Dataset d;
    json cfg = minimal(d);
    cfg["latency_preset"] = "binance_spot_tokyo_vps";
    cfg["latency"] = {{"ack", {{"model", "constant"}, {"us", 500}}}};
    cfg["seed"] = 1;
    cfg["fees"] = {{"maker_rate", "-0.0001"}};
    const RunConfig rc = resolve_run_config(cfg, {"seed=7", "latency.entry.median_us=5000", "strategy.params.size=\"0.002\"",
                                                  "impact.half_life_ms=2.5", "impact.permanent_coefficient=0.5",
                                                  "impact.daily_volatility=0.025", "impact.daily_volume=20000", "latency_regime.curve=[[500,1],[2000,2.5]]",
                                                  "latency_regime.channels=[\"entry\",\"ack\"]",
                                                  "risk.max_position=\"0.01\"", "risk.max_drawdown=25",
                                                  "risk.max_open_orders=4", "gap_policy=flatten"});
    const json& lat = rc.resolved["latency"];
    CHECK(lat["ack"] == json{{"model", "constant"}, {"us", 500}});         // the file's model, nothing of the preset's
    CHECK(lat["entry"]["model"] == "lognormal");                           // the preset's model ...
    CHECK(lat["entry"]["median_us"] == 5000);                              // ... with an overridden parameter
    CHECK(lat["entry"]["sigma"] == 0.3);
    CHECK(lat["feed"] == latency_preset("binance_spot_tokyo_vps")["feed"]);
    CHECK(rc.sim.seed == 7);
    CHECK(rc.sim.impact_half_life_ns == 2500000);
    CHECK(rc.sim.permanent_impact.coefficient == 0.5);
    CHECK(rc.sim.permanent_impact.daily_volatility == 0.025);
    CHECK(rc.sim.permanent_impact.daily_volume == 20000 * kFixedScale);
    const LatencyRegimeConfig& lr = rc.sim.latency_regime;
    CHECK(lr.curve == std::vector<std::pair<double, double>>{{500.0, 1.0}, {2000.0, 2.5}});
    CHECK((!lr.feed && lr.entry && lr.ack));
    CHECK(rc.risk.max_position == 1000000);
    CHECK(rc.risk.max_drawdown == 2500000000LL);
    CHECK(rc.risk.max_open_orders == 4);
    CHECK(rc.risk.max_order_notional == 0);
    CHECK(rc.session().risk.max_open_orders == 4);
    CHECK(rc.session().gap == GapPolicy::FLATTEN);
    CHECK(rc.fees.maker_rate == -10000);   // a rebate
    CHECK(rc.fees.taker_rate == 100000);   // default
    CHECK(rc.strategy_params["size"] == "0.002");
    Rng rng(1);
    CHECK(rc.ack->sample_ns(rng) == 500000);

    // A value that isn't JSON, or is JSON null, is a string
    CHECK(resolve_run_config(cfg, {"strategy.name=null"}).strategy_name == "null");
    CHECK(resolve_run_config(cfg, {"output.dir=results/a b"}).output_dir == "results/a b");
    // An empty object clears
    CHECK(resolve_run_config(cfg, {"strategy.params={}"}).strategy_params == json::object());

    // An override can choose the preset
    const RunConfig z = resolve_run_config(cfg, {"latency_preset=zero"});
    CHECK(z.resolved["latency"]["entry"] == latency_preset("zero")["entry"]);
    CHECK(z.resolved["latency"]["ack"]["us"] == 500);  // the file still beats the preset
}

TEST_CASE("windows: times as ns, dates or UTC timestamps, normalized in the resolved config") {
    Dataset d;
    json cfg = minimal(d);
    cfg["window"] = {{"from", "2026-11-05"}, {"to", "2026-11-05T06:00:00.5Z"}, {"warmup_s", 1.5}};
    const RunConfig rc = resolve_run_config(cfg);
    CHECK(rc.sim.from_ns == 1793836800000000000ULL);
    CHECK(rc.sim.to_ns == 1793836800000000000ULL + 6 * 3600000000000ULL + 500000000ULL);
    CHECK(rc.sim.warmup_ns == 1500000000ULL);
    CHECK(rc.resolved["window"]["from"] == "2026-11-05T00:00:00.000000000Z");
    CHECK(rc.resolved["window"]["to"] == "2026-11-05T06:00:00.500000000Z");

    const RunConfig ns = resolve_run_config(cfg, {"window.from=1793840400000000000"});
    CHECK(ns.sim.from_ns == 1793840400000000000ULL);
    CHECK(ns.resolved["window"]["from"] == "2026-11-05T01:00:00.000000000Z");
}

TEST_CASE("parse_time_ns and format_time_ns") {
    CHECK(parse_time_ns("0") == 0);
    CHECK(parse_time_ns("1970-01-01") == 0);
    CHECK(parse_time_ns("2026-11-05T02:00:00Z") == 1793844000000000000ULL);
    CHECK(parse_time_ns("2026-11-05T02:00:00.000000001Z") == 1793844000000000001ULL);
    CHECK(parse_time_ns("2024-02-29T23:59:59.25Z") == 1709251199250000000ULL);
    CHECK(format_time_ns(1709251199250000000ULL) == "2024-02-29T23:59:59.250000000Z");
    CHECK(format_time_ns(0) == "1970-01-01T00:00:00.000000000Z");
    for (uint64_t t : {1793844000000000001ULL, 951782400123456789ULL, 4102444799999999999ULL}) {
        CHECK(parse_time_ns(format_time_ns(t)) == t);
    }
    for (const char* bad : {"", "2026-11-05T02:00:00", "2026-13-01", "2026-11-05 02:00:00Z", "2026-11-05T02:00:00.Z",
                            "2026-11-05T02:00:00.1234567890Z", "yesterday", "-5"}) {
        CHECK_THROWS_AS(parse_time_ns(bad), std::invalid_argument);
    }
}

TEST_CASE("the resolved config resolves to itself") {
    Dataset d;
    json cfg = minimal(d);
    cfg["window"] = {{"from", "2026-11-05T01:00:00Z"}, {"warmup_s", 60}};
    cfg["queue_model"] = "prob_power:2";
    const RunConfig a = resolve_run_config(cfg, {"latency_preset=binance_spot_home_pc"});
    const RunConfig b = resolve_run_config(a.resolved);
    CHECK(a.resolved == b.resolved);
    CHECK(b.sim.queue_model == "prob_power:2");
    CHECK(b.sim.from_ns == a.sim.from_ns);
}

TEST_CASE("queue models: a name, or an object with model and n") {
    Dataset d;
    json cfg = minimal(d);
    cfg["queue_model"] = "pro_rata";
    CHECK(resolve_run_config(cfg).sim.queue_model == "pro_rata");
    cfg["queue_model"] = {{"model", "prob_power"}, {"n", 3}};
    CHECK(resolve_run_config(cfg).sim.queue_model == "prob_power:3");
    cfg["queue_model"] = {{"model", "no_such"}};
    CHECK(contains(error_of(cfg), "queue_model"));
}

TEST_CASE("datasets: a .bse file directly; a symbol is needed when the manifest has several") {
    Dataset d;
    json cfg = minimal(d);
    cfg["dataset"] = (d.dir / "OTHERUSDT.bse").string();
    cfg.erase("symbol");
    const RunConfig rc = resolve_run_config(cfg);
    CHECK(rc.symbol == "OTHERUSDT");
    CHECK(rc.bse_sha256.empty());
    CHECK(rc.manifest_path.empty());

    cfg["symbol"] = "TESTUSDT";
    CHECK(contains(error_of(cfg), "symbol: 'TESTUSDT' but"));
    cfg["dataset"] = d.str();
    cfg.erase("symbol");
    CHECK(contains(error_of(cfg), "several symbols (TESTUSDT, OTHERUSDT)"));
    cfg["symbol"] = "ETHUSDT";
    CHECK(contains(error_of(cfg), "'ETHUSDT' is not in the dataset"));
    cfg["dataset"] = (d.dir / "missing").string();
    CHECK(contains(error_of(cfg), "dataset: no such file"));
}

TEST_CASE("errors name the key") {
    Dataset d;
    const json cfg = minimal(d);
    auto with = [&](const std::string& o) { return error_of(cfg, {o}); };
    CHECK(contains(with("window.form=5"), "unknown key 'window.form'"));
    CHECK(contains(with("colour=red"), "unknown key 'colour'"));
    CHECK(contains(with("latency.feed.mean=1"), "unknown key 'latency.feed.mean'"));
    CHECK(contains(with("strategy.name=\"\""), "strategy.name: empty"));
    CHECK(contains(with("strategy.owner=MARKET"), "strategy.owner"));
    CHECK(contains(with("tie_break=random"), "tie_break"));
    CHECK(contains(error_of(cfg, {"window.from=2026-11-05T02:00:00Z", "window.to=2026-11-05T01:00:00Z"}),
                   "window.to: must be after"));
    CHECK(contains(error_of(cfg, {"window.from=1000", "window.to=2000", "window.warmup_s=1"}), "window.warmup_s"));
    CHECK(contains(with("window.from=soon"), "window.from: bad time"));
    CHECK(contains(with("fees.taker_rate=abc"), "fees.taker_rate"));
    CHECK(contains(with("fees.taker_rate=\"0.000000001\""), "fees.taker_rate"));
    CHECK(contains(with("fees.taker_rate=2"), "fees.taker_rate: a rate above 1"));
    CHECK(contains(with("limits.open_orders=-1"), "limits.open_orders"));
    CHECK(contains(with("limits.request_weight_per_min=-1"), "limits.request_weight_per_min"));
    CHECK(contains(with("seed=1.5"), "seed"));
    CHECK(contains(with("impact.half_life_ms=-1"), "impact.half_life_ms"));
    CHECK(contains(with("impact.permanent_coefficient=1"), "impact.daily_volatility"));
    CHECK(contains(with("impact.permanent_coefficient=-1"), "impact.permanent_coefficient"));
    CHECK(contains(with("latency_regime.window_ms=0"), "latency_regime.window_ms"));
    CHECK(contains(with("risk.max_position=\"-1\""), "risk.max_position"));
    CHECK(contains(with("risk.max_drawdown=\"0.000000001\""), "risk.max_drawdown"));
    CHECK(contains(with("risk.max_loss=1"), "unknown key 'risk.max_loss'"));
    CHECK(contains(with("gap_policy=pause"), "gap_policy"));
    CHECK(contains(with("latency_regime.curve=[[2,1],[1,2]]"), "latency_regime.curve: latency regime: curve rates"));
    CHECK(contains(with("latency_regime.curve=[1,2]"), "latency_regime.curve"));
    CHECK(contains(with("latency_regime.channels=[\"order\"]"), "latency_regime.channels: 'order'"));
    CHECK(contains(with("latency_preset=fast"), "unknown preset 'fast'"));
    CHECK(contains(with("latency.feed={\"model\":\"warp\"}"), "latency.feed: unknown latency model"));
    CHECK(contains(with("latency.ack={\"model\":\"constant\"}"), "latency.ack"));
    CHECK(contains(with("noequals"), "expected key.path=value"));

    json no_latency = cfg;
    no_latency.erase("latency_preset");
    CHECK(contains(error_of(no_latency), "latency: missing"));
    json no_strategy = cfg;
    no_strategy.erase("strategy");
    CHECK(contains(error_of(no_strategy), "strategy.name: missing"));
    json no_dataset = cfg;
    no_dataset.erase("dataset");
    CHECK(contains(error_of(no_dataset), "dataset: missing"));
    CHECK(contains(error_of(json::array()), "expected a JSON object"));
}

TEST_CASE("load_run_config reads a file") {
    Dataset d;
    const fs::path path = d.dir / "run.json";
    std::ofstream(path) << minimal(d).dump();
    CHECK(load_run_config(path.string(), {"seed=3"}).sim.seed == 3);
    std::ofstream(d.dir / "broken.json") << "{ not json";
    CHECK_THROWS_AS(load_run_config((d.dir / "broken.json").string()), std::invalid_argument);
    CHECK_THROWS_AS(load_run_config((d.dir / "none.json").string()), std::invalid_argument);
}

} // TEST_SUITE("config")
