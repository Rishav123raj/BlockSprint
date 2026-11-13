// LiveFeed (P7.1): the live server's market data through the simulator's Synthesizer. Golden (a) sent as JSON
// messages gives the hand-computed queue and fill, and the committed slice sent as messages builds exactly the
// book the Synthesizer builds from the file.
#include <doctest.h>
#include "latency_models.hpp"
#include "live_feed.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef BLOCKSPRINT_TEST_DATA_DIR
#define BLOCKSPRINT_TEST_DATA_DIR "tests/sim/data"
#endif

using namespace sim;
using nlohmann::json;

namespace {

constexpr int64_t kTick = 1000000;  // 0.01
constexpr int64_t kLot = 100000;    // 0.001
const std::string kSymbol = "TESTUSDT";

struct Server {
    std::shared_ptr<SimClock> clock = std::make_shared<SimClock>(0);
    MatchingEngine engine{clock, EngineOptions{false, false}};
    std::unique_ptr<LiveFeed> feed;
    std::vector<ExecReport> fills;

    explicit Server(LiveFeedOptions o = {}) {
        SymbolSpec spec;
        spec.symbol = kSymbol;
        spec.tick_size = kTick;
        spec.lot_size = kLot;
        spec.min_qty = kLot;
        engine.set_symbol_spec(spec);
        engine.set_default_fee_schedule(binance_spot_regular());
        engine.register_exec_report_callback([this](const ExecReport& r) {
            if (r.owner_id == "C1" && r.type == ExecType::FILL) fills.push_back(r);
        });
        feed = std::make_unique<LiveFeed>(engine, o);
    }

    void at(uint64_t t, const json& m) {
        clock->set(t);
        feed->apply(m);
    }

    std::vector<Trade> buy(const std::string& id, const char* price, const char* qty) {
        auto o = std::make_shared<Order>();
        o->order_id = id;
        o->symbol = kSymbol;
        o->order_type = OrderType::LIMIT;
        o->side = Side::BUY;
        o->price = *parse_fixed(price);
        o->quantity = *parse_fixed(qty);
        o->owner_id = "C1";
        return engine.submit_order(o);
    }

    // The queue at a price: synthetic (+) and client (-) quantities in lots, front first
    std::vector<int64_t> queue(Side side, const char* price) {
        std::vector<int64_t> q;
        const PriceLevel* level = engine.find_level(kSymbol, side, *parse_fixed(price));
        for (const Order* o = level ? level->head : nullptr; o; o = o->queue_next) {
            q.push_back((o->market_data_liquidity ? 1 : -1) * o->remaining_quantity / kLot);
        }
        return q;
    }
};

// [[price, qty], ...] (a braced list of string pairs would make nlohmann build an object)
using Levels = std::vector<std::pair<const char*, const char*>>;
json levels(const Levels& l) {
    json out = json::array();
    for (const auto& [p, q] : l) out.push_back(json::array({p, q}));
    return out;
}
json snapshot(const Levels& bids, const Levels& asks) {
    return {{"type", "snapshot"}, {"symbol", kSymbol}, {"bids", levels(bids)}, {"asks", levels(asks)}};
}
json depth(const Levels& bids, const Levels& asks = {}) {
    return {{"type", "depth"}, {"symbol", kSymbol}, {"bids", levels(bids)}, {"asks", levels(asks)}};
}
json trade(const char* side, const char* price, const char* qty) {
    return {{"type", "trade"}, {"symbol", kSymbol}, {"side", side}, {"price", price}, {"qty", qty}};
}

} // namespace

TEST_SUITE("live_feed") {

TEST_CASE("golden (a) as feed messages: the same queue and fill as from a file") {
    Server s;
    s.at(1, snapshot({{"100.00", "0.010"}}, {{"101.00", "0.004"}}));
    s.buy("c1", "100.00", "0.002");                         // behind the 10 synthetic lots
    s.at(3, depth({{"100.00", "0.012"}}));                   // +2 at the back
    CHECK(s.queue(Side::BUY, "100.00") == std::vector<int64_t>{10, -2, 2});
    s.at(4, trade("sell", "100.00", "0.003"));               // 3 from the front
    s.at(5, depth({{"100.00", "0.009"}}));                   // reflects the trade: nothing more to remove
    CHECK(s.queue(Side::BUY, "100.00") == std::vector<int64_t>{7, -2, 2});
    CHECK(s.fills.empty());
    s.at(6, trade("sell", "100.00", "0.009"));               // 7 synthetic, then our 2
    REQUIRE(s.fills.size() == 1);
    CHECK(s.fills[0].timestamp == 6);
    CHECK(s.fills[0].last_qty == 2 * kLot);
    CHECK(s.fills[0].liquidity == Liquidity::MAKER);
    CHECK(s.fills[0].fee == 20000);
    CHECK(s.queue(Side::BUY, "100.00").empty());

    const json st = s.feed->stats()["symbols"][kSymbol];
    CHECK(st["synced"] == true);
    CHECK(st["messages"] == 5);
    CHECK(st["trades"] == 2);
    CHECK(st["trade_reproduced"] == 0.5);  // the second trade went partly to the client
    CHECK(st["trade_diverted_qty"] == "0.002");
    CHECK(st["top"]["bid"] == "100.0");    // the data still shows 100.00 x 9: its depth event comes next
}

TEST_CASE("the committed slice sent as messages builds the book the Synthesizer builds from the file") {
    Replayer rp(std::string(BLOCKSPRINT_TEST_DATA_DIR) + "/btcusdt_2026-11-05_0200_1m.bse");
    const auto& h = rp.header();
    auto engine_of = [&](std::shared_ptr<SimClock> c) {
        auto e = std::make_unique<MatchingEngine>(c, EngineOptions{false, false});
        SymbolSpec spec;
        spec.symbol = h.symbol;
        spec.tick_size = h.tick_size;
        spec.lot_size = h.lot_size;
        spec.min_qty = h.lot_size;
        e->set_symbol_spec(spec);
        return e;
    };
    auto ca = std::make_shared<SimClock>(0), cb = std::make_shared<SimClock>(0);
    auto ea = engine_of(ca), eb = engine_of(cb);
    LiveFeedOptions o;
    o.queue_model = "prob_power:2";
    o.seed = 5;
    LiveFeed feed(*ea, o);
    auto model = make_queue_model(o.queue_model);
    Rng rng = stream_rng(o.seed, "queue/" + h.symbol);  // the feed's stream for the symbol
    Synthesizer direct(*eb, *model, rng, SynthesizerOptions{h.symbol, h.tick_size, h.lot_size});

    auto px = [&](int64_t ticks) { return format_fixed(ticks * h.tick_size); };
    auto qty = [&](int64_t lots) { return format_fixed(lots * h.lot_size); };
    Batch b;
    uint64_t n = 0, compared = 0;
    while (rp.next(b)) {
        json m{{"symbol", h.symbol}, {"ts", b.begin->ts_exch}};
        if (b.kind == BatchKind::TRADE) {
            m["type"] = "trade";
            m["side"] = b.begin->side == bse::BID ? "buy" : "sell";
            m["price"] = px(b.begin->px);
            m["qty"] = qty(b.begin->qty);
        } else if (b.kind == BatchKind::GAP) {
            m["type"] = "gap";
        } else {
            m["type"] = b.kind == BatchKind::SNAPSHOT ? "snapshot" : "depth";
            json bids = json::array(), asks = json::array();
            for (const bse::Record* r = b.begin; r < b.end; ++r) {
                if (r->kind != bse::SNAP_LEVEL && r->kind != bse::DELTA) continue;
                (r->side == bse::BID ? bids : asks).push_back({px(r->px), qty(r->qty)});
            }
            m["bids"] = bids;
            m["asks"] = asks;
        }
        const auto t = static_cast<uint64_t>(b.ts_local());
        ca->set(t);
        cb->set(t);
        feed.apply(json::parse(m.dump()));  // through the text, as from the socket
        direct.apply(b);
        if (++n % 100 == 0) {
            const Depth da = ea->depth(h.symbol, 20), db = eb->depth(h.symbol, 20);
            REQUIRE(da.bids.size() == db.bids.size());
            REQUIRE(da.asks.size() == db.asks.size());
            for (size_t i = 0; i < da.bids.size(); ++i) {
                CHECK(da.bids[i].price == db.bids[i].price);
                CHECK(da.bids[i].quantity == db.bids[i].quantity);
            }
            for (size_t i = 0; i < da.asks.size(); ++i) {
                CHECK(da.asks[i].price == db.asks[i].price);
                CHECK(da.asks[i].quantity == db.asks[i].quantity);
            }
            ++compared;
        }
    }
    CHECK(compared > 10);
    const SynthStats& a = feed.synthesizer(h.symbol)->stats();
    const SynthStats& d = direct.stats();
    CHECK(a.trades == d.trades);
    CHECK(a.trades_exact == d.trades_exact);
    CHECK(a.cancelled_qty == d.cancelled_qty);
    CHECK(a.added_qty == d.added_qty);
    CHECK(ea->check_invariants(h.symbol).empty());
    // On this slice about two thirds of trades meet exactly their quantity at their price (the rest find
    // stale or iceberg levels: P3.18's gate V4); the feed reports the same share as the direct run
    const uint64_t synced = d.trades - d.trades_unsynced;
    CHECK(feed.stats()["symbols"][h.symbol]["trade_reproduced"].get<double>() ==
          doctest::Approx(static_cast<double>(d.trades_exact) / static_cast<double>(synced)));
}

TEST_CASE("depth before a snapshot is ignored; a gap unsyncs until the next one and is reported once") {
    Server s;
    s.at(1, depth({{"100.00", "0.005"}}));
    CHECK(s.engine.depth(kSymbol, 5).bids.empty());
    s.at(2, snapshot({{"100.00", "0.005"}}, {{"100.10", "0.002"}}));
    s.at(3, json{{"type", "gap"}, {"symbol", kSymbol}});
    CHECK(s.feed->take_gaps() == std::vector<std::string>{kSymbol});
    CHECK(s.feed->take_gaps().empty());
    CHECK(s.engine.depth(kSymbol, 5).bids.empty());       // the synthetic book is flushed
    s.at(4, trade("sell", "100.00", "0.001"));             // not matched while unsynced
    const json st = s.feed->stats()["symbols"][kSymbol];
    CHECK(st["synced"] == false);
    CHECK(st["depth_unsynced"] == 1);
    CHECK(st["trades_unsynced"] == 1);
    CHECK(st["gaps"] == 1);
    s.at(5, snapshot({{"99.00", "0.001"}}, {}));
    CHECK(s.feed->stats()["symbols"][kSymbol]["synced"] == true);
    CHECK(s.engine.depth(kSymbol, 5).bids.size() == 1);
}

TEST_CASE("an array of messages applies in order; a bad one stops it and names its index") {
    Server s;
    json frame = json::array({snapshot({{"100.00", "0.005"}}, {}), depth({{"100.00", "0.007"}}),
                              depth({{"100.005", "0.001"}}), depth({{"100.00", "0.009"}})});
    CHECK_THROWS_WITH_AS(s.at(1, frame), doctest::Contains("(message 2)"), std::invalid_argument);
    CHECK(s.queue(Side::BUY, "100.00") == std::vector<int64_t>{7});  // the first two applied
}

TEST_CASE("malformed messages, unknown symbols and values off the grid are refused") {
    Server s;
    auto error_of = [&](const json& m) {
        try {
            s.at(1, m);
        } catch (const std::invalid_argument& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    auto contains = [](const std::string& a, const std::string& b) { return a.find(b) != std::string::npos; };
    CHECK(contains(error_of(json{{"type", "depth"}, {"symbol", "XUSDT"}}), "unknown symbol 'XUSDT'"));
    CHECK(contains(error_of(json{{"symbol", kSymbol}}), "'type' missing"));
    CHECK(contains(error_of(json{{"type", "quote"}, {"symbol", kSymbol}}), "unknown type 'quote'"));
    CHECK(contains(error_of(json::array({1})), "expected a JSON object"));
    CHECK(contains(error_of(depth({{"100.005", "0.001"}})), "not a positive multiple of the tick 0.01"));
    CHECK(contains(error_of(depth({{"100.00", "0.0015"}})), "not a multiple of the lot 0.001"));
    json short_level = depth({});
    short_level["bids"] = json::array({json::array({"100.00"})});
    CHECK(contains(error_of(short_level), "each level is [price, qty]"));
    json not_list = depth({});
    not_list["bids"] = json{{"100.00", "0.001"}};
    CHECK(contains(error_of(not_list), "must be a list of [price, qty]"));
    CHECK(contains(error_of(depth({{"-1", "0.001"}})), "not a non-negative decimal"));
    CHECK(contains(error_of(trade("both", "100.00", "0.001")), "must be 'buy' or 'sell'"));
    CHECK(contains(error_of(trade("buy", "100.00", "0")), "qty must be positive"));
    json no_price = trade("buy", "100.00", "0.001");
    no_price.erase("price");
    CHECK(contains(error_of(no_price), "needs 'price' and 'qty'"));
    json bad_ts = depth({{"100.00", "0.001"}});
    bad_ts["ts"] = "soon";
    CHECK(contains(error_of(bad_ts), "'ts' must be an integer"));
    CHECK_THROWS_AS(LiveFeed(s.engine, LiveFeedOptions{"no_such_model", 0, 0}), std::invalid_argument);
}

TEST_CASE("reset forgets the symbols; a changed spec starts a symbol over") {
    Server s;
    s.at(1, snapshot({{"100.00", "0.005"}}, {}));
    REQUIRE(s.feed->synthesizer(kSymbol));
    s.engine.reset();
    s.feed->reset();
    CHECK(s.feed->synthesizer(kSymbol) == nullptr);
    s.at(2, snapshot({{"100.00", "0.005"}}, {}));
    CHECK(s.queue(Side::BUY, "100.00") == std::vector<int64_t>{5});

    SymbolSpec coarser;
    coarser.symbol = kSymbol;
    coarser.tick_size = 10 * kTick;
    coarser.lot_size = kLot;
    s.engine.set_symbol_spec(coarser);
    s.at(3, depth({{"100.00", "0.006"}}));                 // a new synthesizer: unsynced, ignored
    CHECK(s.feed->stats()["symbols"][kSymbol]["synced"] == false);
    CHECK(s.feed->stats()["symbols"][kSymbol]["depth_unsynced"] == 1);
}

TEST_CASE("with an impact half-life, liquidity a client took stays out of the book as the data changes") {
    LiveFeedOptions o;
    o.impact_half_life_ns = 1000;
    Server s(o);
    s.at(1, snapshot({}, {{"100.10", "0.010"}}));
    auto o1 = std::make_shared<Order>();
    o1->order_id = "c1";
    o1->symbol = kSymbol;
    o1->order_type = OrderType::IOC;
    o1->side = Side::BUY;
    o1->price = *parse_fixed("100.10");
    o1->quantity = 4 * kLot;
    o1->owner_id = "C1";
    s.feed->client_trades(s.engine.submit_order(o1));       // takes 4 of 10 at t=1
    s.at(1001, depth({}, {{"100.10", "0.010"}}));            // one half-life: 2 still held back
    CHECK(s.queue(Side::SELL, "100.10") == std::vector<int64_t>{8});
    CHECK(s.feed->stats()["symbols"][kSymbol]["impact_taken_qty"] == "0.004");
}

} // TEST_SUITE
