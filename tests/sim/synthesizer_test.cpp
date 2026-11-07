// Queue models (P3.7) and the synthesizer (P3.6). The golden scenarios have hand-computed expected queues
// and fills, worked out in the comments before the code was written.
#include <doctest.h>
#include "queue_models.hpp"
#include "synthesizer.hpp"

#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace sim;

namespace {

// Test market: tick 0.01, lot 0.001. Prices below are in ticks (10000 = 100.00), quantities in lots.
constexpr int64_t kTick = 1000000;
constexpr int64_t kLot = 100000;
const std::string kSymbol = "TESTUSDT";

struct Slot {
    bool synthetic;
    int64_t lots;
    bool operator==(const Slot& o) const { return synthetic == o.synthetic && lots == o.lots; }
};

doctest::String toString(const std::vector<Slot>& q) {
    std::string s = "[";
    for (const auto& slot : q) {
        s += (slot.synthetic ? " syn " : " S1 ") + std::to_string(slot.lots);
    }
    return (s + " ]").c_str();
}

Slot syn(int64_t lots) { return Slot{true, lots}; }
Slot mine(int64_t lots) { return Slot{false, lots}; }

struct Fixture {
    std::shared_ptr<SimClock> clock = std::make_shared<SimClock>(0);
    MatchingEngine engine{clock, EngineOptions{false}};
    std::unique_ptr<QueueModel> model;
    Rng rng{1};
    std::unique_ptr<Synthesizer> synth;
    std::vector<ExecReport> reports;  // the strategy's (owner S1)
    std::deque<std::vector<bse::Record>> batches;  // keeps the records of applied batches alive
    int order_seq = 0;

    explicit Fixture(const std::string& model_spec = "cancel_from_back") : model(make_queue_model(model_spec)) {
        SymbolSpec spec;
        spec.symbol = kSymbol;
        spec.tick_size = kTick;
        spec.lot_size = kLot;
        spec.min_qty = kLot;
        engine.set_symbol_spec(spec);
        engine.set_default_fee_schedule(binance_spot_regular());
        engine.register_exec_report_callback([this](const ExecReport& r) {
            if (r.owner_id == "S1") {
                reports.push_back(r);
            }
        });
        synth = std::make_unique<Synthesizer>(engine, *model, rng, SynthesizerOptions{kSymbol, kTick, kLot});
    }

    void run(BatchKind kind, std::vector<bse::Record> records) {
        clock->set(static_cast<uint64_t>(records.front().ts_local));
        batches.push_back(std::move(records));
        const auto& r = batches.back();
        synth->apply(Batch{kind, r.data(), r.data() + r.size(), 0});
    }

    static bse::Record rec(int64_t ts, uint8_t kind, uint8_t side, int64_t px, int64_t qty, uint16_t flags = 0) {
        return bse::Record{ts, ts, px, qty, 1, kind, side, flags};
    }

    // levels: {side (0 bid, 1 ask), px, lots}
    void snapshot(int64_t ts, std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels) {
        std::vector<bse::Record> r{rec(ts, bse::SNAP_BEGIN, 0, 0, 0, bse::FROM_SNAPSHOT)};
        for (auto [side, px, qty] : levels) {
            r.push_back(rec(ts, bse::SNAP_LEVEL, side, px, qty, bse::FROM_SNAPSHOT));
        }
        r.push_back(rec(ts, bse::SNAP_END, 0, 0, 0, bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH));
        run(BatchKind::SNAPSHOT, std::move(r));
    }

    void depth(int64_t ts, std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels) {
        std::vector<bse::Record> r;
        for (auto [side, px, qty] : levels) {
            r.push_back(rec(ts, bse::DELTA, side, px, qty));
        }
        r.back().flags = bse::LAST_IN_BATCH;
        run(BatchKind::DEPTH, std::move(r));
    }

    // aggressor: 0 buyer (consumes asks), 1 seller (consumes bids)
    void trade(int64_t ts, uint8_t aggressor, int64_t px, int64_t lots) {
        run(BatchKind::TRADE, {rec(ts, bse::TRADE, aggressor, px, lots)});
    }

    void gap(int64_t ts) { run(BatchKind::GAP, {rec(ts, bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH)}); }

    std::string send(Side side, OrderType type, int64_t px, int64_t lots) {
        auto o = std::make_shared<Order>();
        o->order_id = "s" + std::to_string(++order_seq);
        o->symbol = kSymbol;
        o->order_type = type;
        o->side = side;
        o->price = px * kTick;
        o->quantity = lots * kLot;
        o->owner_id = "S1";
        engine.submit_order(o);
        return o->order_id;
    }
    std::string buy(int64_t px, int64_t lots) { return send(Side::BUY, OrderType::LIMIT, px, lots); }
    std::string sell(int64_t px, int64_t lots) { return send(Side::SELL, OrderType::LIMIT, px, lots); }

    std::vector<Slot> queue(Side side, int64_t px) {
        std::vector<Slot> q;
        const PriceLevel* level = engine.find_level(kSymbol, side, px * kTick);
        for (const Order* o = level ? level->head : nullptr; o; o = o->queue_next) {
            q.push_back(Slot{o->market_data_liquidity, o->remaining_quantity / kLot});
        }
        return q;
    }

    std::vector<ExecReport> fills() const {
        std::vector<ExecReport> f;
        for (const auto& r : reports) {
            if (r.type == ExecType::FILL) {
                f.push_back(r);
            }
        }
        return f;
    }

    void consistent(bool exact = false) {
        auto violations = synth->check(exact);
        CHECK_MESSAGE(violations.empty(), (violations.empty() ? "" : violations.front()));
        auto engine_violations = engine.check_invariants(kSymbol);
        CHECK_MESSAGE(engine_violations.empty(), (engine_violations.empty() ? "" : engine_violations.front()));
    }
};

constexpr uint8_t BID = 0, ASK = 1;
constexpr uint8_t AGGR_BUY = 0, AGGR_SELL = 1;

} // namespace

TEST_SUITE("queue_models") {

std::vector<int64_t> choose(const std::string& spec, const std::vector<QueueSlot>& q, int64_t amount, int64_t unit = 1) {
    auto model = make_queue_model(spec);
    Rng rng(1);
    std::vector<int64_t> take;
    model->choose(q, amount, unit, rng, take);
    return take;
}

TEST_CASE("each model splits a cancel of 5 over [syn 6][S1 2][syn 4] as computed by hand") {
    const std::vector<QueueSlot> q{{6, true}, {2, false}, {4, true}};
    // back: behind first (4), then 1 ahead
    CHECK(choose("cancel_from_back", q, 5) == std::vector<int64_t>{1, 0, 4});
    // front: 5 of the 6 ahead
    CHECK(choose("cancel_from_front", q, 5) == std::vector<int64_t>{5, 0, 0});
    // pro rata: 5 x 6/10 = 3, 5 x 4/10 = 2
    CHECK(choose("pro_rata", q, 5) == std::vector<int64_t>{3, 0, 2});
    // power 1: share ahead 6/10, 3 of 5
    CHECK(choose("prob_power:1", q, 5) == std::vector<int64_t>{3, 0, 2});
    // power 3: 1 / (1 + (4/6)^3) = 0.7714, 5 x 0.7714 = 3.86 -> 4
    CHECK(choose("prob_power:3", q, 5) == std::vector<int64_t>{4, 0, 1});
    // log: ln 7 / (ln 7 + ln 5) = 0.5473, 2.74 -> 3
    CHECK(choose("prob_log", q, 5) == std::vector<int64_t>{3, 0, 2});
}

TEST_CASE("pro rata rounds by largest remainder in whole lots; ties go to the newer order") {
    // 3 x 6/10 = 1.8, 3 x 4/10 = 1.2: floors 1 + 1, the extra lot to the larger remainder (front)
    CHECK(choose("pro_rata", {{6, true}, {2, false}, {4, true}}, 3) == std::vector<int64_t>{2, 0, 1});
    // 1 x 1/2 each: a tie, the newer one gives
    CHECK(choose("pro_rata", {{5, true}, {5, true}}, 1) == std::vector<int64_t>{0, 1});
    // in units of 100: 300 over 600 / 400 -> 200 / 100
    CHECK(choose("pro_rata", {{600, true}, {400, true}}, 300, 100) == std::vector<int64_t>{200, 100});
}

TEST_CASE("strategy slots are never chosen, and nothing exceeds a slot or the synthetic total") {
    const std::vector<QueueSlot> q{{3, true}, {5, false}, {2, true}, {1, false}, {4, true}};
    for (const char* spec : {"cancel_from_back", "cancel_from_front", "pro_rata", "prob_power:2", "prob_log"}) {
        CAPTURE(spec);
        for (int64_t amount : {0, 1, 4, 9, 20}) {
            CAPTURE(amount);
            auto take = choose(spec, q, amount);
            REQUIRE(take.size() == q.size());
            int64_t total = 0;
            for (size_t i = 0; i < q.size(); ++i) {
                CHECK(take[i] >= 0);
                CHECK(take[i] <= q[i].qty);
                if (!q[i].synthetic) {
                    CHECK(take[i] == 0);
                }
                total += take[i];
            }
            CHECK(total == std::min<int64_t>(amount, 9));
        }
    }
}

TEST_CASE("probabilistic models: one side empty, and no strategy order in the level") {
    // nothing ahead: all behind
    CHECK(choose("prob_power:2", {{2, false}, {5, true}}, 3) == std::vector<int64_t>{0, 3});
    // nothing behind: all ahead
    CHECK(choose("prob_power:2", {{5, true}, {2, false}}, 3) == std::vector<int64_t>{3, 0});
    // the share ahead doesn't fit: the rest comes from behind
    CHECK(choose("prob_power:1", {{1, true}, {2, false}, {9, true}}, 8) == std::vector<int64_t>{1, 0, 7});
    // no strategy order: from the back
    CHECK(choose("prob_log", {{4, true}, {4, true}}, 5) == std::vector<int64_t>{1, 4});
}

TEST_CASE("model names and parsing") {
    CHECK(make_queue_model("prob_power:2")->name() == "prob_power:2");
    CHECK(make_queue_model("prob_power:0.5")->name() == "prob_power:0.5");
    CHECK(make_queue_model("prob_log")->name() == "prob_log");
    CHECK(make_queue_model("pro_rata")->name() == "pro_rata");
    CHECK_THROWS_AS(make_queue_model("prob_power:"), std::invalid_argument);
    CHECK_THROWS_AS(make_queue_model("prob_power:-1"), std::invalid_argument);
    CHECK_THROWS_AS(make_queue_model("prob_power:2x"), std::invalid_argument);
    CHECK_THROWS_AS(make_queue_model("fifo"), std::invalid_argument);
}

TEST_CASE("det_exp and det_pow match the library to 1e-13") {
    for (double x : {-700.0, -20.0, -1.0, -1e-9, 0.0, 1e-9, 0.5, 1.0, 2.302585, 20.0, 700.0}) {
        CAPTURE(x);
        CHECK(std::abs(det_exp(x) - std::exp(x)) <= 1e-13 * std::exp(x));
    }
    CHECK(std::abs(det_pow(2.0, 10.0) - 1024.0) < 1e-10);
    CHECK(std::abs(det_pow(4.0 / 6.0, 3.0) - 0.2962962962962963) < 1e-15);
}

} // TEST_SUITE

TEST_SUITE("synthesizer") {

TEST_CASE("golden (a): trades consume the queue ahead, then fill the strategy order") {
    Fixture f;
    f.snapshot(1, {{BID, 10000, 10}, {ASK, 10100, 4}});
    f.buy(10000, 2);                      // joins behind the 10 synthetic lots
    f.depth(3, {{BID, 10000, 12}});       // +2 at the back
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(10), mine(2), syn(2)});

    f.trade(4, AGGR_SELL, 10000, 3);      // 3 from the front
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(7), mine(2), syn(2)});
    f.depth(5, {{BID, 10000, 9}});        // the depth event reflecting the trade: nothing more to remove
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(7), mine(2), syn(2)});
    CHECK(f.fills().empty());

    // 9 lots: 7 synthetic, then all 2 of ours. The real trade took 9 of real liquidity, so 2 more come off
    // the front of the synthetic queue: the level is empty, as the next depth event says
    f.trade(6, AGGR_SELL, 10000, 9);
    auto fills = f.fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].timestamp == 6);
    CHECK(fills[0].last_px == 10000 * kTick);
    CHECK(fills[0].last_qty == 2 * kLot);
    CHECK(fills[0].liquidity == Liquidity::MAKER);
    CHECK(fills[0].leaves_qty == 0);
    CHECK(fills[0].fee == 20000);  // 0.002 x 100.00 x 0.1% = 0.0002
    CHECK(f.queue(Side::BUY, 10000).empty());
    f.depth(7, {{BID, 10000, 0}});
    f.consistent(true);

    const auto& s = f.synth->stats();
    CHECK(s.tape_synthetic_qty == 10 * kLot);
    CHECK(s.tape_strategy_qty == 2 * kLot);
    CHECK(s.trade_front_qty == 2 * kLot);
    CHECK(s.cancelled_qty == 0);
}

TEST_CASE("golden (b): an unexplained cancel of 5 under each queue model, then a trade of 4") {
    // [syn 6][S1 2][syn 4], level 10 -> 5. The fill from the trade of 4 = 2 - max(0, ahead + 2 - 4) ...
    // i.e. min(2, max(0, 4 - ahead)).
    struct Case {
        const char* model;
        std::vector<Slot> after_cancel;
        int64_t filled;
    };
    const std::vector<Case> cases{
        {"cancel_from_back", {syn(5), mine(2)}, 0},          // ahead 5: the trade doesn't reach us
        {"cancel_from_front", {syn(1), mine(2), syn(4)}, 2}, // ahead 1: filled
        {"pro_rata", {syn(3), mine(2), syn(2)}, 1},          // ahead 3: 1 lot
        {"prob_power:1", {syn(3), mine(2), syn(2)}, 1},
        {"prob_power:3", {syn(2), mine(2), syn(3)}, 2},      // ahead 2: filled
        {"prob_log", {syn(3), mine(2), syn(2)}, 1},
    };
    for (const auto& c : cases) {
        CAPTURE(c.model);
        Fixture f(c.model);
        f.snapshot(1, {{BID, 10000, 6}, {ASK, 10100, 4}});
        f.buy(10000, 2);
        f.depth(3, {{BID, 10000, 10}});
        CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(6), mine(2), syn(4)});
        f.depth(4, {{BID, 10000, 5}});
        CHECK(f.queue(Side::BUY, 10000) == c.after_cancel);
        CHECK(f.synth->stats().cancelled_qty == 5 * kLot);
        f.trade(5, AGGR_SELL, 10000, 4);
        int64_t filled = 0;
        for (const auto& r : f.fills()) {
            filled += r.last_qty;
        }
        CHECK(filled == c.filled * kLot);
        f.consistent();
    }
}

TEST_CASE("golden (c): a trade and the depth event reflecting it are not counted twice") {
    Fixture f("cancel_from_front");  // the optimistic model: double counting would show at once
    f.snapshot(1, {{BID, 10000, 10}, {ASK, 10100, 4}});
    f.buy(10000, 2);
    f.trade(3, AGGR_SELL, 10000, 4);
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(6), mine(2)});
    f.depth(4, {{BID, 10000, 6}});        // 10 - 4: explained by the trade, ahead stays 6 (not 2)
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(6), mine(2)});
    CHECK(f.synth->stats().cancelled_qty == 0);
    f.depth(5, {{BID, 10000, 8}});        // growth joins the back
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(6), mine(2), syn(2)});
    f.trade(6, AGGR_SELL, 10000, 1);
    f.depth(7, {{BID, 10000, 5}});        // 8 - 1 trade - 2 cancelled: only the 2 go to the model (front)
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(3), mine(2), syn(2)});
    CHECK(f.synth->stats().cancelled_qty == 2 * kLot);
    CHECK(f.fills().empty());
    f.consistent();
}

TEST_CASE("golden (d): data moving through a resting strategy order fills it at the order's price") {
    Fixture f;
    f.snapshot(1, {{BID, 10000, 5}, {ASK, 10200, 5}});
    f.sell(10100, 3);                     // rests between bid 100.00 and ask 102.00
    f.depth(3, {{BID, 10150, 2}});        // a bid at 101.50 appears: it crosses our ask at 101.00
    auto fills = f.fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].last_px == 10100 * kTick);
    CHECK(fills[0].last_qty == 2 * kLot);
    CHECK(fills[0].liquidity == Liquidity::MAKER);
    CHECK(fills[0].leaves_qty == 1 * kLot);
    CHECK(f.queue(Side::BUY, 10150).empty());  // the 2 lots traded with us
    f.consistent();

    f.depth(4, {{BID, 10150, 3}});        // the level changes: 3 lots added, 1 trades with our last lot
    fills = f.fills();
    REQUIRE(fills.size() == 2);
    CHECK(fills[1].last_px == 10100 * kTick);
    CHECK(fills[1].last_qty == 1 * kLot);
    CHECK(fills[1].leaves_qty == 0);
    CHECK(f.queue(Side::BUY, 10150) == std::vector<Slot>{syn(2)});
    CHECK(f.synth->stats().crossing_fills == 2);
    CHECK(f.synth->stats().crossing_qty == 3 * kLot);
    f.consistent();
    CHECK_FALSE(f.engine.is_crossed(kSymbol));
}

TEST_CASE("golden (f): an IOC through 3 levels pays taker fees on each; the data restores on change") {
    Fixture f;
    f.snapshot(1, {{BID, 10000, 1}, {ASK, 10100, 2}, {ASK, 10200, 3}, {ASK, 10300, 5}});
    f.send(Side::BUY, OrderType::IOC, 10300, 7);
    // 2 @ 101.00, 3 @ 102.00, 2 @ 103.00; fee 0.1% of 0.202, 0.306 and 0.206
    auto fills = f.fills();
    REQUIRE(fills.size() == 3);
    CHECK(fills[0].last_px == 10100 * kTick);
    CHECK(fills[0].last_qty == 2 * kLot);
    CHECK(fills[0].fee == 20200);
    CHECK(fills[1].last_px == 10200 * kTick);
    CHECK(fills[1].last_qty == 3 * kLot);
    CHECK(fills[1].fee == 30600);
    CHECK(fills[2].last_px == 10300 * kTick);
    CHECK(fills[2].last_qty == 2 * kLot);
    CHECK(fills[2].fee == 20600);
    CHECK(fills[2].cum_fee == 71400);
    CHECK(fills[2].liquidity == Liquidity::TAKER);
    CHECK(fills[2].leaves_qty == 0);
    CHECK(f.queue(Side::SELL, 10300) == std::vector<Slot>{syn(3)});
    f.consistent();  // 3 synthetic lots under the data's 5: allowed

    f.depth(3, {{ASK, 10300, 4}});        // impact half-life 0: the level is set to the data when it changes
    CHECK(f.queue(Side::SELL, 10300) == std::vector<Slot>{syn(4)});
    CHECK(f.queue(Side::SELL, 10100).empty());  // unchanged levels keep the impact
}

TEST_CASE("golden (g): a gap flushes synthetic liquidity, nothing matches until the snapshot") {
    Fixture f;
    f.snapshot(1, {{BID, 10000, 5}, {ASK, 10100, 5}});
    f.buy(10000, 2);
    f.gap(3);
    CHECK_FALSE(f.synth->synced());
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{mine(2)});  // our order stays
    CHECK(f.queue(Side::SELL, 10100).empty());

    f.trade(4, AGGR_SELL, 10000, 10);     // would reach us, but the book is unknown: not matched
    f.depth(5, {{ASK, 9950, 1}});         // ignored
    CHECK(f.fills().empty());
    CHECK(f.synth->stats().trades_unsynced == 1);
    CHECK(f.synth->stats().depth_unsynced == 1);

    // The market moved down through our bid while we weren't looking: the resync snapshot's ask at 99.50
    // trades with our bid at its price, 100.00
    f.snapshot(6, {{BID, 9900, 3}, {ASK, 9950, 5}});
    CHECK(f.synth->synced());
    auto fills = f.fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].timestamp == 6);
    CHECK(fills[0].last_px == 10000 * kTick);
    CHECK(fills[0].last_qty == 2 * kLot);
    CHECK(f.queue(Side::SELL, 9950) == std::vector<Slot>{syn(3)});
    CHECK(f.queue(Side::BUY, 9900) == std::vector<Slot>{syn(3)});
    f.consistent();
}

TEST_CASE("a snapshot without a gap reconciles: strategy orders keep their place") {
    Fixture f;
    f.snapshot(1, {{BID, 10000, 5}, {BID, 9900, 1}, {ASK, 10100, 5}});
    f.buy(10000, 2);
    f.depth(3, {{BID, 10000, 8}});
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(5), mine(2), syn(3)});
    f.snapshot(4, {{BID, 10000, 8}, {ASK, 10100, 5}});  // same level; 99.00 is gone
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(5), mine(2), syn(3)});
    CHECK(f.queue(Side::BUY, 9900).empty());
    f.snapshot(5, {{BID, 10000, 6}, {ASK, 10100, 5}});  // shrinks by 2: the model (back) takes it behind us
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(5), mine(2), syn(1)});
    f.consistent();
}

TEST_CASE("a trade through a better-priced strategy order fills it, and the trade price level loses the rest") {
    Fixture f;
    f.snapshot(1, {{BID, 10000, 5}, {ASK, 10100, 5}});
    f.buy(10050, 1);                      // alone at 100.50, above the data's best bid
    f.trade(3, AGGR_SELL, 10000, 3);      // a seller hits 100.00 for 3: it would have hit us first
    auto fills = f.fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].last_px == 10050 * kTick);
    CHECK(fills[0].last_qty == 1 * kLot);
    // 2 lots from the IOC, 1 from the front for the lot we took: 100.00 lost the trade's 3
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(2)});
    f.depth(4, {{BID, 10000, 2}});
    f.consistent(true);
}

TEST_CASE("synthetic orders between strategy orders are merged only when adjacent") {
    Fixture f;
    f.snapshot(1, {{BID, 10000, 1}, {ASK, 10100, 5}});
    f.depth(2, {{BID, 10000, 2}});
    f.depth(3, {{BID, 10000, 4}});
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(4)});
    f.buy(10000, 1);
    f.depth(4, {{BID, 10000, 5}});
    f.depth(5, {{BID, 10000, 7}});
    CHECK(f.queue(Side::BUY, 10000) == std::vector<Slot>{syn(4), mine(1), syn(3)});
}

TEST_CASE("without strategy orders the synthetic book equals the data after every book batch (random streams)") {
    for (const char* spec : {"cancel_from_back", "cancel_from_front", "pro_rata", "prob_power:2", "prob_log"}) {
        CAPTURE(spec);
        Fixture f(spec);
        Rng rng(99);
        std::map<int64_t, int64_t> bids, asks;  // the stream's own book (px -> lots)
        int64_t ts = 1;
        auto random_snapshot = [&]() {
            bids.clear();
            asks.clear();
            std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels;
            for (int i = 0; i < 8; ++i) {
                bids[9990 - static_cast<int64_t>(rng.below(20))] = 1 + static_cast<int64_t>(rng.below(20));
                asks[10010 + static_cast<int64_t>(rng.below(20))] = 1 + static_cast<int64_t>(rng.below(20));
            }
            for (auto [px, q] : bids) levels.emplace_back(BID, px, q);
            for (auto [px, q] : asks) levels.emplace_back(ASK, px, q);
            f.snapshot(ts++, levels);
        };
        random_snapshot();
        for (int step = 0; step < 3000; ++step) {
            const uint64_t what = rng.below(100);
            if (what < 2) {
                f.gap(ts++);
                random_snapshot();
            } else {
                std::vector<std::tuple<uint8_t, int64_t, int64_t>> changes;
                if (what < 40 && !bids.empty() && !asks.empty()) {
                    // trades at the best price, then the depth event that reflects them (plus noise)
                    const bool sell = rng.below(2) == 0;
                    auto& book = sell ? bids : asks;
                    const int64_t px = sell ? bids.rbegin()->first : asks.begin()->first;
                    const int64_t q = 1 + static_cast<int64_t>(rng.below(static_cast<uint64_t>(book[px])));
                    f.trade(ts, sell ? AGGR_SELL : AGGR_BUY, px, q);
                    book[px] -= q;
                    changes.emplace_back(sell ? BID : ASK, px, book[px]);
                    if (book[px] == 0) book.erase(px);
                }
                for (int k = 1 + static_cast<int>(rng.below(3)); k > 0; --k) {
                    const bool bid = rng.below(2) == 0;
                    auto& book = bid ? bids : asks;
                    const int64_t px = bid ? 9990 - static_cast<int64_t>(rng.below(20))
                                           : 10010 + static_cast<int64_t>(rng.below(20));
                    const int64_t q = rng.below(4) == 0 ? 0 : 1 + static_cast<int64_t>(rng.below(20));
                    bool dup = false;
                    for (const auto& c : changes) dup |= std::get<0>(c) == (bid ? BID : ASK) && std::get<1>(c) == px;
                    if (dup) continue;
                    if (q == 0) book.erase(px); else book[px] = q;
                    changes.emplace_back(bid ? BID : ASK, px, q);
                }
                if (!changes.empty()) f.depth(ts, changes);
                ++ts;
            }
            auto violations = f.synth->check(true);
            REQUIRE_MESSAGE(violations.empty(), "step " << step << ": " << (violations.empty() ? "" : violations.front()));
            REQUIRE(f.engine.check_invariants(kSymbol).empty());
        }
        CHECK(f.synth->stats().trades > 500);
        CHECK(f.synth->stats().cancelled_qty > 0);
        CHECK(f.synth->stats().gaps > 10);
    }
}

TEST_CASE("with strategy orders the synthetic book never exceeds the data and the engine stays consistent") {
    for (const char* spec : {"cancel_from_back", "pro_rata", "prob_power:2"}) {
        CAPTURE(spec);
        Fixture f(spec);
        Rng rng(7);
        f.snapshot(1, {{BID, 9990, 10}, {BID, 9980, 10}, {ASK, 10010, 10}, {ASK, 10020, 10}});
        int64_t ts = 2;
        for (int step = 0; step < 3000; ++step) {
            const uint64_t what = rng.below(100);
            const int64_t px_bid = 9970 + static_cast<int64_t>(rng.below(30));
            const int64_t px_ask = 10001 + static_cast<int64_t>(rng.below(30));
            const int64_t q = 1 + static_cast<int64_t>(rng.below(10));
            if (what < 10) {
                f.buy(9975 + static_cast<int64_t>(rng.below(40)), q);  // may cross: then it takes
            } else if (what < 20) {
                f.sell(9995 + static_cast<int64_t>(rng.below(40)), q);
            } else if (what < 45) {
                f.trade(ts++, rng.below(2) ? AGGR_SELL : AGGR_BUY, rng.below(2) ? px_bid : px_ask, q);
            } else if (what < 47) {
                f.gap(ts++);
                f.snapshot(ts++, {{BID, px_bid, q}, {ASK, px_ask, q}});
            } else {
                // a depth event that never crosses the data's own book
                const auto& data = f.synth->data();
                auto bb = data.best_bid();
                auto ba = data.best_ask();
                if (rng.below(2) == 0) {
                    if (!ba || px_bid < ba->px) f.depth(ts++, {{BID, px_bid, rng.below(3) ? q : 0}});
                } else {
                    if (!bb || px_ask > bb->px) f.depth(ts++, {{ASK, px_ask, rng.below(3) ? q : 0}});
                }
            }
            auto violations = f.synth->check(false);
            REQUIRE_MESSAGE(violations.empty(), "step " << step << ": " << (violations.empty() ? "" : violations.front()));
            auto engine_violations = f.engine.check_invariants(kSymbol);
            REQUIRE_MESSAGE(engine_violations.empty(),
                            "step " << step << ": " << (engine_violations.empty() ? "" : engine_violations.front()));
        }
        CHECK(f.synth->stats().tape_strategy_qty > 0);
        CHECK(f.synth->stats().crossing_qty > 0);
        CHECK(f.synth->stats().would_cross_qty == 0);
    }
}

} // TEST_SUITE
