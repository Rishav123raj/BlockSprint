// Determinism (P1.17): the same operation script replayed into fresh engines gives bit-identical output.
//
// A script (a list of operations with their simulated times) is generated once from a seed and then
// replayed. The hash covers everything the engine emits, in order: every execution report (all fields,
// including seq, timestamp and rejection text), every trade, every BBO and depth callback, and the return
// value of every call. Each item is serialized canonically through the same JSON the API sends
// (nlohmann::json orders keys, so dump() is stable) and folded into FNV-1a.
//
// The golden value pins the stream across compilers and platforms: CI checks it on MSVC/Windows and
// GCC/Linux. It changes only when the engine's output changes on purpose; update it in the same commit
// and say why.
#include <doctest.h>
#include "workload.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

enum class OpKind { SUBMIT, REPLACE, CANCEL, MODIFY, AMEND };

struct Op {
    uint64_t at_ns = 0;  // simulated time of the call
    OpKind kind = OpKind::SUBMIT;
    std::string symbol;
    std::string order_id;
    OrderType type = OrderType::LIMIT;
    Side side = Side::BUY;
    Price price = 0;
    Quantity quantity = 0;  // submit/replace: quantity; modify: new open quantity; amend: new total
    std::optional<Price> amend_price;
    std::string owner;
    StpPolicy policy = StpPolicy::CANCEL_MAKER;
    bool market_data = false;
};

const std::vector<std::string> kSymbols = {"DET-A", "DET-B", "DET-SPEC"};

// DET-SPEC has trading rules, so the script also produces tick, lot, range and notional rejections
SymbolSpec det_spec() {
    SymbolSpec s;
    s.symbol = "DET-SPEC";
    s.tick_size = fx(0.01);
    s.lot_size = fx(0.001);
    s.min_qty = fx(0.001);
    s.max_qty = fx(5.0);
    s.min_notional = fx(5.0);
    return s;
}

std::vector<Op> make_script(uint64_t seed, int ops, uint64_t start_ns) {
    Lcg rng{seed};
    auto pick = [&](uint32_t n) { return rng.next() % n; };
    std::vector<Op> script;
    std::vector<std::pair<std::string, std::string>> ids;
    uint64_t now = start_ns;

    for (int i = 0; i < ops; ++i) {
        now += 1 + pick(5) * 250000;  // up to 1 ms apart; sometimes 1 ns
        Op op;
        op.at_ns = now;
        uint32_t action = pick(100);
        op.kind = action < 60 ? OpKind::SUBMIT : action < 65 ? OpKind::REPLACE : action < 82 ? OpKind::CANCEL
                                             : action < 91 ? OpKind::MODIFY : OpKind::AMEND;
        if ((op.kind != OpKind::SUBMIT && op.kind != OpKind::REPLACE) && ids.empty()) {
            op.kind = OpKind::SUBMIT;
        }
        // Prices around 100 in 0.005 steps: half of them are off DET-SPEC's 0.01 tick
        Price px = fx(100.0) + (static_cast<int64_t>(pick(41)) - 20) * 500000;
        Quantity q = (1 + static_cast<int64_t>(pick(64))) * 50000;  // 0.0005 .. 0.032 (some off-lot)
        if (pick(4) == 0) {
            q *= 100;  // up to 3.2
        }

        if (op.kind == OpKind::SUBMIT || op.kind == OpKind::REPLACE) {
            op.symbol = kSymbols[pick(3)];
            uint32_t t = pick(100);
            op.type = t < 45 ? OrderType::LIMIT : t < 58 ? OrderType::LIMIT_MAKER : t < 70 ? OrderType::MARKET
                    : t < 85 ? OrderType::IOC : OrderType::FOK;
            op.side = pick(2) ? Side::BUY : Side::SELL;
            op.price = op.type == OrderType::MARKET ? 0 : px;
            op.quantity = pick(50) == 0 ? q * 400 : q;  // rarely up to 12.8: above DET-SPEC's max of 5
            uint32_t who = pick(10);
            if (who < 2) {
                op.owner = "MARKET";
                op.policy = StpPolicy::SKIP;
                op.market_data = true;
            } else if (who < 9) {
                op.owner = "P" + std::to_string(pick(3));
                op.policy = static_cast<StpPolicy>(pick(5));
            }
            if (op.kind == OpKind::REPLACE) {
                op.order_id = ids[ids.size() - 1 - pick(static_cast<uint32_t>(std::min<size_t>(ids.size(), 20)))].second;
            } else {
                op.order_id = "D" + std::to_string(i);
            }
            ids.emplace_back(op.symbol, op.order_id);
        } else {
            const auto& target = ids[ids.size() - 1 - pick(static_cast<uint32_t>(std::min<size_t>(ids.size(), 20)))];
            op.symbol = target.first;
            op.order_id = target.second;
            op.quantity = q;
            op.price = px;
            if (op.kind == OpKind::AMEND && pick(3) == 0) {
                op.amend_price.reset();
            } else {
                op.amend_price = px;
            }
        }
        script.push_back(op);
    }
    return script;
}

std::string canonical(const Op& op) {
    return std::to_string(op.at_ns) + "|" + std::to_string(static_cast<int>(op.kind)) + "|" + op.symbol + "|" +
           op.order_id + "|" + orderTypeToString(op.type) + "|" + sideToString(op.side) + "|" +
           std::to_string(op.price) + "|" + std::to_string(op.quantity) + "|" +
           (op.amend_price ? std::to_string(*op.amend_price) : "-") + "|" + op.owner + "|" +
           stpPolicyToString(op.policy) + "|" + (op.market_data ? "md" : "");
}

struct Replay {
    uint64_t hash = 14695981039346656037ULL;
    size_t reports = 0;
    size_t trades = 0;
    size_t bbo_updates = 0;
    size_t depth_updates = 0;
    std::map<std::string, size_t> report_kinds;  // "type/reason"
    std::vector<std::string> first_items;  // the first few serialized items, to show a divergence
};

Replay replay(const std::vector<Op>& script, uint64_t start_ns, EngineOptions options = {}) {
    auto clock = std::make_shared<SimClock>(start_ns);
    MatchingEngine eng(clock, options);
    eng.set_default_fee_schedule(binance_spot_regular());
    eng.set_fee_schedule("P1", FeeSchedule{-5000, 40000});  // maker rebate
    eng.set_symbol_spec(det_spec());

    Replay out;
    auto fold = [&out](const std::string& item) {
        out.hash = fnv1a(out.hash, item);
        out.hash = fnv1a(out.hash, "\n");
        if (out.first_items.size() < 50) {
            out.first_items.push_back(item);
        }
    };
    eng.register_exec_report_callback([&](const ExecReport& r) {
        ++out.reports;
        ++out.report_kinds[execTypeToString(r.type) + "/" + execReasonToString(r.reason)];
        fold("R" + nlohmann::json(r).dump());
    });
    eng.register_trade_callback([&](const std::string& s, const Trade& t) {
        ++out.trades;
        fold("T" + s + nlohmann::json(t).dump());
    });
    eng.register_bbo_callback([&](const std::string& s, const Bbo& b) {
        ++out.bbo_updates;
        fold("B" + s + bbo_to_json(s, b).dump());
    });
    eng.register_depth_callback([&](const std::string& s, const Depth& d) {
        ++out.depth_updates;
        fold("L" + s + depth_to_json(s, d).dump());
    });

    for (const Op& op : script) {
        clock->set(op.at_ns);
        std::vector<Trade> trades;
        bool ok = true;
        switch (op.kind) {
            case OpKind::SUBMIT:
            case OpKind::REPLACE: {
                auto order = std::make_shared<Order>();
                order->order_id = op.order_id;
                order->symbol = op.symbol;
                order->order_type = op.type;
                order->side = op.side;
                order->price = op.price;
                order->quantity = op.quantity;
                order->owner_id = op.owner;
                order->stp_policy = op.policy;
                order->market_data_liquidity = op.market_data;
                order->timestamp = op.at_ns;
                trades = op.kind == OpKind::SUBMIT ? eng.submit_order(order) : eng.replace_order(order);
                break;
            }
            case OpKind::CANCEL:
                ok = eng.cancel_order(op.symbol, op.order_id);
                break;
            case OpKind::MODIFY:
                ok = eng.modify_order(op.symbol, op.order_id, op.quantity, op.price, trades);
                break;
            case OpKind::AMEND:
                ok = eng.amend_order(op.symbol, op.order_id, op.quantity, op.amend_price, trades);
                break;
        }
        fold("=" + std::to_string(ok) + "/" + std::to_string(trades.size()));
    }
    // Final state of every book, all levels
    for (const auto& s : kSymbols) {
        fold("F" + depth_to_json(s, eng.depth(s, 1 << 20)).dump());
    }
    return out;
}

constexpr uint64_t kDetSeed = 20261105ULL;
constexpr int kDetOps = 4000;
constexpr uint64_t kDetStart = 1793836800000000000ULL;  // 2026-11-05T00:00:00Z

// Pinned at P1.17. See the header comment before changing it.
constexpr uint64_t kGoldenReportStreamHash = 2815580882014666384ULL;

} // namespace

TEST_SUITE("determinism") {

TEST_CASE("the script is a pure function of its seed") {
    auto a = make_script(kDetSeed, 500, kDetStart);
    auto b = make_script(kDetSeed, 500, kDetStart);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        REQUIRE(canonical(a[i]) == canonical(b[i]));
    }
}

TEST_CASE("replaying the same script into fresh engines gives a bit-identical output stream") {
    const auto script = make_script(kDetSeed, kDetOps, kDetStart);
    Replay first = replay(script, kDetStart);
    Replay second = replay(script, kDetStart);
    Replay unlocked = replay(script, kDetStart, EngineOptions{false});

    CHECK(first.first_items == second.first_items);
    CHECK(first.hash == second.hash);
    CHECK(first.reports == second.reports);
    CHECK(unlocked.hash == first.hash);  // locking doesn't change results
    MESSAGE("report-stream hash = " << first.hash << " (" << first.reports << " reports, " << first.trades
                                    << " trades, " << first.bbo_updates << " BBO and " << first.depth_updates
                                    << " depth updates)");

    // The script must exercise the engine broadly, or the hash pins little
    CHECK(first.trades > 500U);
    CHECK(first.reports > 5000U);
    for (const char* kind : {"fill/", "amended/", "cancelled/replaced", "cancelled/stp_maker", "cancelled/fok_kill",
                             "rejected/price_tick", "rejected/quantity_lot", "rejected/quantity_range",
                             "rejected/min_notional", "rejected/post_only_would_take"}) {
        CAPTURE(kind);
        CHECK(first.report_kinds[kind] > 0U);
    }

    CHECK(first.hash == kGoldenReportStreamHash);
}

TEST_CASE("the hash is sensitive to the input") {
    const auto script = make_script(kDetSeed, 1000, kDetStart);
    const uint64_t base = replay(script, kDetStart).hash;

    CHECK(replay(make_script(kDetSeed + 1, 1000, kDetStart), kDetStart).hash != base);

    // Shifting every time by 1 ns changes only timestamps
    auto shifted = script;
    for (auto& op : shifted) {
        op.at_ns += 1;
    }
    CHECK(replay(shifted, kDetStart).hash != base);

    // One operation fewer at the end
    auto shorter = script;
    shorter.pop_back();
    CHECK(replay(shorter, kDetStart).hash != base);
}

} // TEST_SUITE("determinism")
