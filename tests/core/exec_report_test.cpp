// Execution reports (P1.10): every order ends in an explicit fill, cancel-with-reason or reject.
#include <doctest.h>
#include "workload.hpp"
#include <map>
#include <set>

namespace {

struct Expected {
    ExecType type;
    ExecReason reason;
    std::string order_id;
};

// Compares type, reason and order of a report sequence; prints the actual sequence on mismatch
void check_sequence(const std::vector<ExecReport>& actual, const std::vector<Expected>& expected) {
    std::string got;
    for (const auto& r : actual) {
        got += execTypeToString(r.type) + (r.reason != ExecReason::NONE ? "(" + execReasonToString(r.reason) + ")" : "") +
               ":" + r.order_id + " ";
    }
    CAPTURE(got);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(actual[i].type == expected[i].type);
        CHECK(actual[i].reason == expected[i].reason);
        CHECK(actual[i].order_id == expected[i].order_id);
    }
}

// Checks the report contract (events.hpp) on a live report stream, then against the book at the end
struct LifecycleChecker {
    struct State {
        bool open = false;
        Quantity cum = 0;
        int64_t fees = 0;
    };
    std::map<std::string, State> orders;
    // Fee schedules in force (P1.13); each fill's fee is recomputed independently from them
    const FeeBook* fee_book = nullptr;
    std::map<std::string, std::vector<ExecReport>> fills_by_trade;
    uint64_t last_seq = 0;
    std::vector<std::string> errors;

    void fail(const ExecReport& r, const std::string& what) {
        if (errors.size() < 10) {
            errors.push_back("seq " + std::to_string(r.seq) + " " + execTypeToString(r.type) + " " + r.order_id +
                             ": " + what);
        }
    }

    void on(const ExecReport& r) {
        if (r.seq != last_seq + 1) {
            fail(r, "sequence gap after " + std::to_string(last_seq));
        }
        last_seq = r.seq;
        State& st = orders[r.order_id];

        if (r.type == ExecType::REJECTED) {
            // A rejected order never existed; a duplicate ID is rejected because the original is open
            if (r.reason == ExecReason::DUPLICATE_ORDER_ID && !st.open) {
                fail(r, "duplicate rejected but no order with that ID is open");
            }
            if (r.leaves_qty != 0) {
                fail(r, "rejected with open quantity");
            }
            return;
        }
        if (r.type == ExecType::ACK) {
            if (st.open) {
                fail(r, "ACK for an order that is already open");
            }
            if (r.cum_qty != 0 || r.leaves_qty != r.quantity) {
                fail(r, "ACK must have cum 0 and leaves == quantity");
            }
            if (r.cum_fee != 0) {
                fail(r, "ACK with fees");
            }
            st = State{true, 0, 0};
            return;
        }
        if (!st.open) {
            fail(r, "report after the order's terminal report (or before its ACK)");
            return;
        }
        if (r.type == ExecType::FILL) {
            if (r.last_qty <= 0 || r.cum_qty != st.cum + r.last_qty) {
                fail(r, "cum_qty does not add up");
            }
            fills_by_trade[r.trade_id].push_back(r);
            if (r.cum_fee != st.fees + r.fee) {
                fail(r, "cum_fee does not add up");
            }
            if (fee_book) {
                const FeeSchedule& sched = fee_book->schedule_for(r.owner_id);
                int64_t rate = r.liquidity == Liquidity::TAKER ? sched.taker_rate : sched.maker_rate;
                if (r.fee != fee_for(r.last_px, r.last_qty, rate)) {
                    fail(r, "fee " + format_fixed(r.fee) + " differs from the owner's schedule");
                }
            }
        } else if (r.cum_qty != st.cum) {
            fail(r, "cum_qty changed without a fill");
        } else if (r.cum_fee != st.fees) {
            fail(r, "cum_fee changed without a fill");
        }
        if (r.type == ExecType::CANCELLED ? r.leaves_qty != 0 : r.cum_qty + r.leaves_qty != r.quantity) {
            fail(r, "cum + leaves != quantity");
        }
        st.cum = r.cum_qty;
        st.fees = r.cum_fee;
        st.open = !is_terminal(r);
    }

    // After the run: fills pair up per trade, and open orders are exactly the resting ones
    void finish(MatchingEngine& eng, const std::string& symbol, size_t expected_trades) {
        if (fills_by_trade.size() != expected_trades) {
            errors.push_back("trades with fills: " + std::to_string(fills_by_trade.size()) + ", trades executed: " +
                             std::to_string(expected_trades));
        }
        for (const auto& [trade_id, fills] : fills_by_trade) {
            bool ok = fills.size() == 2 && fills[0].liquidity == Liquidity::TAKER &&
                      fills[1].liquidity == Liquidity::MAKER && fills[0].last_px == fills[1].last_px &&
                      fills[0].last_qty == fills[1].last_qty && fills[0].side != fills[1].side;
            if (!ok) {
                errors.push_back("trade " + trade_id + ": fills are not one taker + one maker of equal size");
            }
        }
        for (const auto& [order_id, st] : orders) {
            // Cancelling succeeds exactly for resting orders
            bool resting = eng.cancel_order(symbol, order_id);
            if (resting != st.open) {
                errors.push_back("order " + order_id + (st.open ? " has no terminal report but is not resting"
                                                                 : " is resting after its terminal report"));
            }
        }
    }
};

} // namespace

TEST_SUITE("exec-reports") {

TEST_CASE_FIXTURE(EngineFixture, "a resting limit order is acknowledged and stays open") {
    std::vector<ExecReport> r;
    engine().submit_order(make_order("A", "ER", OrderType::LIMIT, Side::BUY, 100.0, 2.0), &r);
    check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "A"}});
    CHECK(r[0].seq == 1U);
    CHECK(r[0].quantity == fx(2.0));
    CHECK(r[0].leaves_qty == fx(2.0));
    CHECK(r[0].cum_qty == 0);
    CHECK(r[0].price == fx(100.0));
}

TEST_CASE_FIXTURE(EngineFixture, "fills: taker then maker per trade, with running cum and leaves") {
    submit("M1", "ER", OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    submit("M2", "ER", OrderType::LIMIT, Side::SELL, 101.0, 3.0);

    std::vector<ExecReport> r;
    auto trades = engine().submit_order(make_order("T", "ER", OrderType::LIMIT, Side::BUY, 101.0, 2.5), &r);
    REQUIRE(trades.size() == 2U);
    check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "T"},
                       {ExecType::FILL, ExecReason::NONE, "T"},
                       {ExecType::FILL, ExecReason::NONE, "M1"},
                       {ExecType::FILL, ExecReason::NONE, "T"},
                       {ExecType::FILL, ExecReason::NONE, "M2"}});

    // First trade: 1.0 at 100, which completes M1
    CHECK(r[1].liquidity == Liquidity::TAKER);
    CHECK(r[1].last_px == fx(100.0));
    CHECK(r[1].last_qty == fx(1.0));
    CHECK(r[1].cum_qty == fx(1.0));
    CHECK(r[1].leaves_qty == fx(1.5));
    CHECK(r[1].trade_id == trades[0].trade_id);
    CHECK(r[2].liquidity == Liquidity::MAKER);
    CHECK(r[2].leaves_qty == 0);  // terminal for M1
    CHECK(is_terminal(r[2]));

    // Second trade: 1.5 at 101, which completes the taker and leaves M2 with 1.5
    CHECK(r[3].last_px == fx(101.0));
    CHECK(r[3].last_qty == fx(1.5));
    CHECK(r[3].leaves_qty == 0);
    CHECK(is_terminal(r[3]));
    CHECK(r[4].cum_qty == fx(1.5));
    CHECK(r[4].leaves_qty == fx(1.5));
    CHECK_FALSE(is_terminal(r[4]));
}

TEST_CASE_FIXTURE(EngineFixture, "a partially filled limit order rests without a terminal report") {
    submit("M", "ER", OrderType::LIMIT, Side::SELL, 100.0, 1.0);
    std::vector<ExecReport> r;
    engine().submit_order(make_order("T", "ER", OrderType::LIMIT, Side::BUY, 100.0, 3.0), &r);
    REQUIRE(r.size() == 3U);
    CHECK(r[1].order_id == "T");
    CHECK(r[1].leaves_qty == fx(2.0));
    CHECK_FALSE(is_terminal(r[1]));
}

TEST_CASE_FIXTURE(EngineFixture, "IOC remainder, unfilled market and killed FOK are cancelled with a reason") {
    submit("M", "ER", OrderType::LIMIT, Side::SELL, 100.0, 1.0);

    SUBCASE("IOC") {
        std::vector<ExecReport> r;
        engine().submit_order(make_order("I", "ER", OrderType::IOC, Side::BUY, 100.0, 3.0), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "I"},
                           {ExecType::FILL, ExecReason::NONE, "I"},
                           {ExecType::FILL, ExecReason::NONE, "M"},
                           {ExecType::CANCELLED, ExecReason::IOC_REMAINDER, "I"}});
        CHECK(r[3].cum_qty == fx(1.0));
        CHECK(r[3].leaves_qty == 0);
    }
    SUBCASE("MARKET with liquidity left over is not cancelled") {
        std::vector<ExecReport> r;
        engine().submit_order(make_order("K", "ER", OrderType::MARKET, Side::BUY, 0.0, 0.5), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "K"},
                           {ExecType::FILL, ExecReason::NONE, "K"},
                           {ExecType::FILL, ExecReason::NONE, "M"}});
    }
    SUBCASE("MARKET larger than the book") {
        std::vector<ExecReport> r;
        engine().submit_order(make_order("K", "ER", OrderType::MARKET, Side::BUY, 0.0, 5.0), &r);
        CHECK(r.back().type == ExecType::CANCELLED);
        CHECK(r.back().reason == ExecReason::NO_LIQUIDITY);
        CHECK(r.back().cum_qty == fx(1.0));
    }
    SUBCASE("MARKET on an empty side") {
        std::vector<ExecReport> r;
        engine().submit_order(make_order("K", "ER", OrderType::MARKET, Side::SELL, 0.0, 1.0), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "K"},
                           {ExecType::CANCELLED, ExecReason::NO_LIQUIDITY, "K"}});
    }
    SUBCASE("FOK that cannot fill") {
        std::vector<ExecReport> r;
        engine().submit_order(make_order("F", "ER", OrderType::FOK, Side::BUY, 100.0, 2.0), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "F"},
                           {ExecType::CANCELLED, ExecReason::FOK_KILL, "F"}});
        CHECK(r[1].cum_qty == 0);
    }
    SUBCASE("FOK that fills") {
        std::vector<ExecReport> r;
        engine().submit_order(make_order("F", "ER", OrderType::FOK, Side::BUY, 100.0, 1.0), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "F"},
                           {ExecType::FILL, ExecReason::NONE, "F"},
                           {ExecType::FILL, ExecReason::NONE, "M"}});
        CHECK(is_terminal(r[1]));
    }
}

TEST_CASE_FIXTURE(EngineFixture, "self-trade prevention reports who was cancelled and why") {
    auto own = [&](const std::string& id, Side side, double price, StpPolicy policy) {
        auto o = make_order(id, "ER", OrderType::LIMIT, side, price, 1.0);
        o->owner_id = "A";
        o->stp_policy = policy;
        return o;
    };
    engine().submit_order(own("REST", Side::SELL, 100.0, StpPolicy::NONE));

    SUBCASE("cancel_maker") {
        std::vector<ExecReport> r;
        engine().submit_order(own("T", Side::BUY, 100.0, StpPolicy::CANCEL_MAKER), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "T"},
                           {ExecType::CANCELLED, ExecReason::STP_MAKER, "REST"}});
        CHECK(r[1].owner_id == "A");
    }
    SUBCASE("cancel_taker") {
        std::vector<ExecReport> r;
        engine().submit_order(own("T", Side::BUY, 100.0, StpPolicy::CANCEL_TAKER), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "T"},
                           {ExecType::CANCELLED, ExecReason::STP_TAKER, "T"}});
    }
    SUBCASE("cancel_both") {
        std::vector<ExecReport> r;
        engine().submit_order(own("T", Side::BUY, 100.0, StpPolicy::CANCEL_BOTH), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "T"},
                           {ExecType::CANCELLED, ExecReason::STP_MAKER, "REST"},
                           {ExecType::CANCELLED, ExecReason::STP_TAKER, "T"}});
    }
    SUBCASE("skip, remainder would cross (D7)") {
        std::vector<ExecReport> r;
        engine().submit_order(own("T", Side::BUY, 100.0, StpPolicy::SKIP), &r);
        check_sequence(r, {{ExecType::ACK, ExecReason::NONE, "T"},
                           {ExecType::CANCELLED, ExecReason::WOULD_CROSS, "T"}});
    }
}

TEST_CASE_FIXTURE(EngineFixture, "rejections: duplicate ID and zero quantity") {
    submit("A", "ER", OrderType::LIMIT, Side::BUY, 100.0, 1.0);

    std::vector<ExecReport> r;
    engine().submit_order(make_order("A", "ER", OrderType::LIMIT, Side::BUY, 99.0, 1.0), &r);
    check_sequence(r, {{ExecType::REJECTED, ExecReason::DUPLICATE_ORDER_ID, "A"}});
    CHECK(r[0].text.find("Duplicate order_id 'A'") != std::string::npos);
    CHECK(r[0].price == fx(99.0));  // describes the rejected order, not the resting one

    r.clear();
    engine().submit_order(make_order("Z", "ER", OrderType::LIMIT, Side::BUY, 99.0, 0.0), &r);
    check_sequence(r, {{ExecType::REJECTED, ExecReason::INVALID_QUANTITY, "Z"}});
    CHECK(engine().check_invariants("ER").empty());
}

TEST_CASE_FIXTURE(EngineFixture, "cancel, replace and modify") {
    submit("A", "ER", OrderType::LIMIT, Side::BUY, 100.0, 3.0);
    submit("S", "ER", OrderType::LIMIT, Side::SELL, 100.0, 1.0);  // fills 1.0 of A

    SUBCASE("cancel reports the fills so far; an unknown ID reports nothing") {
        std::vector<ExecReport> r;
        CHECK(engine().cancel_order("ER", "A", &r));
        check_sequence(r, {{ExecType::CANCELLED, ExecReason::USER, "A"}});
        CHECK(r[0].cum_qty == fx(1.0));
        r.clear();
        CHECK_FALSE(engine().cancel_order("ER", "A", &r));
        CHECK(r.empty());
    }
    SUBCASE("replace cancels the old order and acknowledges the new one") {
        std::vector<ExecReport> r;
        engine().replace_order(make_order("A", "ER", OrderType::LIMIT, Side::BUY, 99.0, 5.0), &r);
        check_sequence(r, {{ExecType::CANCELLED, ExecReason::REPLACED, "A"}, {ExecType::ACK, ExecReason::NONE, "A"}});
        CHECK(r[0].cum_qty == fx(1.0));
        CHECK(r[1].cum_qty == 0);  // a new order
        CHECK(r[1].leaves_qty == fx(5.0));
    }
    SUBCASE("modify down at the same price keeps priority: one AMENDED report") {
        std::vector<ExecReport> r;
        std::vector<Trade> t;
        CHECK(engine().modify_order("ER", "A", fx(1.5), fx(100.0), t, &r));
        check_sequence(r, {{ExecType::AMENDED, ExecReason::NONE, "A"}});
        CHECK(r[0].leaves_qty == fx(1.5));
        CHECK(r[0].cum_qty == fx(1.0));
        CHECK(r[0].quantity == fx(2.5));
    }
    SUBCASE("modify through the spread: AMENDED, then fills, fills so far are kept") {
        submit("S2", "ER", OrderType::LIMIT, Side::SELL, 101.0, 1.0);
        std::vector<ExecReport> r;
        std::vector<Trade> t;
        CHECK(engine().modify_order("ER", "A", fx(2.0), fx(101.0), t, &r));
        check_sequence(r, {{ExecType::AMENDED, ExecReason::NONE, "A"},
                           {ExecType::FILL, ExecReason::NONE, "A"},
                           {ExecType::FILL, ExecReason::NONE, "S2"}});
        CHECK(r[0].cum_qty == fx(1.0));
        CHECK(r[1].cum_qty == fx(2.0));
        CHECK(r[1].leaves_qty == fx(1.0));
    }
    SUBCASE("modify to zero cancels") {
        std::vector<ExecReport> r;
        std::vector<Trade> t;
        CHECK(engine().modify_order("ER", "A", 0, fx(100.0), t, &r));
        check_sequence(r, {{ExecType::CANCELLED, ExecReason::USER, "A"}});
    }
}

TEST_CASE("reports are sequenced per engine, timestamped by its clock and sent to the callback") {
    auto clock = std::make_shared<SimClock>(5000);
    MatchingEngine eng(clock);
    std::vector<ExecReport> via_callback;
    eng.register_exec_report_callback([&](const ExecReport& r) { via_callback.push_back(r); });

    std::vector<ExecReport> via_out;
    eng.submit_order(make_order("S", "X", OrderType::LIMIT, Side::SELL, 100.0, 1.0), &via_out);
    clock->advance(10);
    eng.submit_order(make_order("B", "Y", OrderType::LIMIT, Side::BUY, 100.0, 1.0), &via_out);
    eng.submit_order(make_order("T", "X", OrderType::MARKET, Side::BUY, 0.0, 1.0), &via_out);

    REQUIRE(via_out.size() == via_callback.size());
    for (size_t i = 0; i < via_out.size(); ++i) {
        CHECK(via_out[i].seq == i + 1);  // one sequence across symbols
        CHECK(via_callback[i].seq == via_out[i].seq);
    }
    CHECK(via_out[0].timestamp == 5000U);
    CHECK(via_out.back().timestamp == 5010U);
}

TEST_CASE("report JSON uses exact decimals and only includes fields that apply") {
    MatchingEngine eng;
    std::vector<ExecReport> r;
    eng.submit_order(make_order("S", "X", OrderType::LIMIT, Side::SELL, 100.5, 0.00000001), &r);
    auto taker = make_order("T", "X", OrderType::IOC, Side::BUY, 101.0, 2.0);
    taker->owner_id = "bob";
    eng.submit_order(taker, &r);

    nlohmann::json ack = r[0];
    CHECK(ack["type"] == "ack");
    CHECK(ack["price"] == "100.5");
    CHECK(ack["quantity"] == "0.00000001");
    CHECK_FALSE(ack.contains("reason"));
    CHECK_FALSE(ack.contains("last_px"));
    CHECK_FALSE(ack.contains("owner"));

    nlohmann::json fill = r[2];
    CHECK(fill["type"] == "fill");
    CHECK(fill["liquidity"] == "taker");
    CHECK(fill["last_px"] == "100.5");
    CHECK(fill["owner"] == "bob");

    nlohmann::json cancelled = r.back();
    CHECK(cancelled["type"] == "cancelled");
    CHECK(cancelled["reason"] == "ioc_remainder");
    CHECK(cancelled["leaves_qty"] == "0.0");
}

TEST_CASE("lifecycle contract holds on random workloads") {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        CAPTURE(seed);
        MatchingEngine eng;
        LifecycleChecker checker;
        // Every other seed runs with real fee schedules: Binance's regular tier by default, a VIP tier,
        // and a maker rebate, so fees are checked on every fill
        FeeBook fees;
        if (seed % 2 == 0) {
            fees.set_default(binance_spot_regular());
            fees.set("P1", FeeSchedule{2000, 4000});      // 0.02% / 0.04%
            fees.set("P2", FeeSchedule{-500, 3000});      // -0.005% rebate / 0.03%
            eng.set_default_fee_schedule(fees.default_schedule());
            eng.set_fee_schedule("P1", *fees.own_schedule("P1"));
            eng.set_fee_schedule("P2", *fees.own_schedule("P2"));
        }
        checker.fee_book = &fees;
        eng.register_exec_report_callback([&](const ExecReport& r) { checker.on(r); });
        // Every STP policy is exercised: mirrored orders skip, the others get random owners and policies
        // Every third seed (seed % 3 == 0) also mixes in post-only orders and Binance-style amends
        WorkloadStats stats =
            run_random_workload(eng, "LC", seed, 2000, {}, seed % 2 ? 15 : 50, true, seed % 3 == 0);
        eng.register_exec_report_callback({});
        checker.finish(eng, "LC", stats.trades);
        for (const auto& e : checker.errors) {
            FAIL_CHECK(e);
        }
        CHECK(checker.errors.empty());
    }
}

} // TEST_SUITE("exec-reports")
