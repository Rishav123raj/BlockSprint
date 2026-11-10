// Golden scenarios end to end (P3.16, architecture §15): each tests/sim/golden/*.golden file is a hand-written
// market (written to a .bse file), a strategy's actions at fixed times, and what must come out, computed by
// hand before the run. The whole simulator runs it: replayer, synthesizer, engine, latency, gateway.
//
// Format, one statement per line, '#' starts a comment. Times are in ms (decimals allowed); prices and
// quantities are decimals on the test market (tick 0.01, lot 0.001).
//
//   latency <ms>                      feed, entry and ack latency, constant (default 1)
//   queue <model> [<model>...]        run once per queue model (default cancel_from_back)
//   fees <maker> <taker>              rates (default 0.001 0.001)
//   end <ms>                          the run ends here (required)
//   data <ms> snapshot|depth <bid|ask> <px> <qty> [<bid|ask> <px> <qty>...]
//   data <ms> trade <buy|sell> <px> <qty>    (the aggressor's side)
//   data <ms> gap
//   send <ms> <ref> <buy|sell> <limit|limit_maker|ioc|fok|market> <px> <qty>
//   cancel <ms> <ref>
//   replace <ms> <ref> <px> <qty>
//   amend <ms> <ref> <qty> [<px>]
//   expect <exchange ms> <delivered ms> <ref> <report>   every report and refused request, in order:
//       ack | fill <px> <qty> <maker|taker> fee <fee> leaves <qty> | cancelled <reason> | rejected <reason>
//       | amended <px> <qty> | refused <cancel|amend|replace>
//   queue_at <ms> <bid|ask> <px> = <queue>   the engine's queue at that price after everything at <ms>:
//       "syn <qty> | S1 <qty> | ..." from the front (adjacent synthetic orders merged), or "empty"
//   final position <qty> cash <amount> fees <amount>   the exchange's account at the end
//
// A statement can be limited to one queue model: expect@pro_rata ..., queue_at@cancel_from_front ...
#include <doctest.h>
#include "sim_helpers.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef BLOCKSPRINT_GOLDEN_DIR
#define BLOCKSPRINT_GOLDEN_DIR "tests/sim/golden"
#endif

using namespace simtest;

namespace {

constexpr int64_t kTick = 1000000;  // 0.01

std::string ms_text(uint64_t ns) {
    if (ns % kMs == 0) return std::to_string(ns / kMs);
    return format_fixed(static_cast<int64_t>(ns) * 100);  // ms in 1e-8 units
}

// Exactly `decimals` places
std::string dec(int64_t v, int decimals) {
    std::string s = format_fixed(v);
    const auto dot = s.find('.');
    std::string frac = s.substr(dot + 1);
    frac.resize(static_cast<size_t>(decimals), '0');
    return s.substr(0, dot) + "." + frac;
}
std::string px_text(int64_t v) { return dec(v, 2); }
std::string qty_text(int64_t v) { return dec(v, 3); }

struct Statement {
    int line = 0;
    std::string variant;  // empty: every queue model
    std::vector<std::string> t;
};

struct Golden {
    std::string name;
    double latency_ms = 1;
    std::vector<std::string> queues{"cancel_from_back"};
    FeeSchedule fees{100000, 100000};
    uint64_t end_ns = 0;
    Stream data;
    std::vector<Statement> actions;    // send, cancel, replace, amend
    std::vector<Statement> queue_at;
    std::vector<Statement> expects;    // the text after "expect"
    std::vector<Statement> finals;
};

[[noreturn]] void fail(const Golden& g, int line, const std::string& what) {
    throw std::runtime_error(g.name + ":" + std::to_string(line) + ": " + what);
}

int64_t fixed(const Golden& g, int line, const std::string& text) {
    const bool negative = !text.empty() && text[0] == '-';
    auto v = parse_fixed(negative ? text.substr(1) : text);
    if (!v) fail(g, line, "not a decimal: " + text);
    return negative ? -*v : *v;
}

uint64_t time_ns(const Golden& g, int line, const std::string& text) {
    const int64_t v = fixed(g, line, text);  // ms in 1e-8 units
    if (v < 0 || v % 100) fail(g, line, "bad time: " + text);
    return static_cast<uint64_t>(v / 100);
}

int64_t in_units(const Golden& g, int line, const std::string& text, int64_t unit) {
    const int64_t v = fixed(g, line, text);
    if (v % unit) fail(g, line, text + " is not a whole number of ticks or lots");
    return v / unit;
}

uint8_t side_code(const Golden& g, int line, const std::string& s) {
    if (s == "bid" || s == "buy") return 0;
    if (s == "ask" || s == "sell") return 1;
    fail(g, line, "expected bid/ask or buy/sell: " + s);
}

Golden parse(const fs::path& path) {
    Golden g;
    g.name = path.filename().string();
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::string text;
    int line = 0;
    while (std::getline(in, text)) {
        ++line;
        if (auto hash = text.find('#'); hash != std::string::npos) text.erase(hash);
        std::istringstream ss(text);
        Statement st;
        st.line = line;
        for (std::string w; ss >> w;) st.t.push_back(w);
        if (st.t.empty()) continue;
        std::string kw = st.t[0];
        if (auto at = kw.find('@'); at != std::string::npos) {
            st.variant = kw.substr(at + 1);
            kw = kw.substr(0, at);
        }
        const auto& t = st.t;
        auto need = [&](size_t n) {
            if (t.size() < n) fail(g, line, "too few fields for " + kw);
        };
        if (kw == "latency") {
            need(2);
            g.latency_ms = std::stod(t[1]);
        } else if (kw == "queue") {
            need(2);
            g.queues.assign(t.begin() + 1, t.end());
        } else if (kw == "fees") {
            need(3);
            g.fees = FeeSchedule{fixed(g, line, t[1]), fixed(g, line, t[2])};
        } else if (kw == "end") {
            need(2);
            g.end_ns = time_ns(g, line, t[1]);
        } else if (kw == "data") {
            need(3);
            const auto ts = static_cast<int64_t>(time_ns(g, line, t[1]));
            const std::string& what = t[2];
            if (what == "snapshot" || what == "depth") {
                if ((t.size() - 3) % 3 != 0 || t.size() == 3) fail(g, line, "levels are <bid|ask> <px> <qty>");
                std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels;
                for (size_t i = 3; i < t.size(); i += 3) {
                    levels.emplace_back(side_code(g, line, t[i]), in_units(g, line, t[i + 1], kTick),
                                        in_units(g, line, t[i + 2], kLot));
                }
                if (what == "snapshot") {
                    g.data.r.push_back(Stream::rec(ts, bse::SNAP_BEGIN, 0, 0, 0, bse::FROM_SNAPSHOT));
                    for (auto [s, px, q] : levels) {
                        g.data.r.push_back(Stream::rec(ts, bse::SNAP_LEVEL, s, px, q, bse::FROM_SNAPSHOT));
                    }
                    g.data.r.push_back(Stream::rec(ts, bse::SNAP_END, 0, 0, 0, bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH));
                } else {
                    g.data.depth_ns(ts, levels);
                }
            } else if (what == "trade") {
                need(6);
                g.data.trade_ns(ts, side_code(g, line, t[3]), in_units(g, line, t[4], kTick),
                                in_units(g, line, t[5], kLot));
            } else if (what == "gap") {
                g.data.r.push_back(Stream::rec(ts, bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH));
            } else {
                fail(g, line, "unknown data kind " + what);
            }
        } else if (kw == "send" || kw == "cancel" || kw == "replace" || kw == "amend") {
            need(kw == "send" ? 7 : kw == "cancel" ? 3 : kw == "replace" ? 5 : 4);
            g.actions.push_back(st);
        } else if (kw == "queue_at") {
            need(6);
            g.queue_at.push_back(st);
        } else if (kw == "expect") {
            need(5);
            g.expects.push_back(st);
        } else if (kw == "final") {
            need(7);
            g.finals.push_back(st);
        } else {
            fail(g, line, "unknown statement " + kw);
        }
    }
    if (g.end_ns == 0) fail(g, line, "no 'end'");
    if (g.data.r.empty()) fail(g, line, "no data");
    return g;
}

std::string join(const std::vector<std::string>& v, size_t from, const char* sep = " ") {
    std::string s;
    for (size_t i = from; i < v.size(); ++i) s += (i > from ? sep : "") + v[i];
    return s;
}

bool applies(const Statement& st, const std::string& model) { return st.variant.empty() || st.variant == model; }

OrderType order_type(const Golden& g, int line, const std::string& s) {
    if (s == "limit") return OrderType::LIMIT;
    if (s == "limit_maker") return OrderType::LIMIT_MAKER;
    if (s == "ioc") return OrderType::IOC;
    if (s == "fok") return OrderType::FOK;
    if (s == "market") return OrderType::MARKET;
    fail(g, line, "unknown order type " + s);
}

// Runs the script and writes down what happened, in the expect / queue_at / final formats
class Runner final : public Strategy, public GatewayListener {
public:
    Runner(const Golden& g, Simulator& sim, const std::string& model) : g_(g), sim_(sim), model_(model) {}

    std::vector<std::string> reports;
    std::vector<std::string> queues;

    std::string name() const override { return "golden"; }

    void on_start(Context& ctx) override {
        for (size_t i = 0; i < g_.actions.size(); ++i) schedule(ctx, g_.actions[i], i);
        for (size_t i = 0; i < g_.queue_at.size(); ++i) {
            if (applies(g_.queue_at[i], model_)) schedule(ctx, g_.queue_at[i], kCheck + i);
        }
    }

    void on_timer(Context& ctx, uint64_t id) override {
        const size_t what = timers_.at(id);
        if (what >= kCheck) {
            queues.push_back(queue_text(g_.queue_at[what - kCheck]));
            return;
        }
        const Statement& st = g_.actions[what];
        const auto& t = st.t;
        const std::string kw = t[0];
        if (kw == "send") {
            const OrderId order = ctx.send(side_code(g_, st.line, t[3]) == 0 ? Side::BUY : Side::SELL,
                                        order_type(g_, st.line, t[4]), fixed(g_, st.line, t[5]),
                                        fixed(g_, st.line, t[6]));
            ids_[t[2]] = order;
            refs_[order] = t[2];
        } else if (kw == "cancel") {
            ctx.cancel(id_of(st, t[2]));
        } else if (kw == "replace") {
            ctx.replace(id_of(st, t[2]), fixed(g_, st.line, t[3]), fixed(g_, st.line, t[4]));
        } else {
            std::optional<Price> price;
            if (t.size() > 4) price = fixed(g_, st.line, t[4]);
            ctx.amend(id_of(st, t[2]), fixed(g_, st.line, t[3]), price);
        }
    }

    void on_report(uint32_t, const ExecReport& r, uint64_t deliver_at) override {
        std::string s = ms_text(r.timestamp) + " " + ms_text(deliver_at) + " " + ref(r.order_id) + " ";
        switch (r.type) {
            case ExecType::ACK: s += "ack"; break;
            case ExecType::FILL:
                s += "fill " + px_text(r.last_px) + " " + qty_text(r.last_qty) + " " +
                     (r.liquidity == Liquidity::MAKER ? "maker" : "taker") + " fee " + format_fixed(r.fee) +
                     " leaves " + qty_text(r.leaves_qty);
                break;
            case ExecType::AMENDED: s += "amended " + px_text(r.price) + " " + qty_text(r.quantity); break;
            case ExecType::CANCELLED: s += "cancelled " + execReasonToString(r.reason); break;
            case ExecType::REJECTED: s += "rejected " + execReasonToString(r.reason); break;
        }
        reports.push_back(s);
    }

    void on_request_reject(uint32_t, const RequestReject& r, uint64_t deliver_at) override {
        reports.push_back(ms_text(r.exchange_time) + " " + ms_text(deliver_at) + " " + ref(r.order_id) + " refused " +
                          request_name(r.request));
    }

private:
    static constexpr size_t kCheck = 1000000;

    void schedule(Context& ctx, const Statement& st, size_t what) {
        const uint64_t at = time_ns(g_, st.line, st.t[1]);
        if (at < ctx.now()) fail(g_, st.line, "before the start");
        timers_[ctx.set_timer(at - ctx.now())] = what;
    }

    const OrderId& id_of(const Statement& st, const std::string& r) const {
        auto it = ids_.find(r);
        if (it == ids_.end()) fail(g_, st.line, "unknown order " + r);
        return it->second;
    }

    std::string ref(const OrderId& id) const {
        auto it = refs_.find(id);
        return it == refs_.end() ? id : it->second;
    }

    std::string queue_text(const Statement& st) const {
        const auto& t = st.t;
        const Side side = side_code(g_, st.line, t[2]) == 0 ? Side::BUY : Side::SELL;
        const PriceLevel* level = sim_.engine().find_level("TESTUSDT", side, fixed(g_, st.line, t[3]));
        std::vector<std::string> parts;
        int64_t syn = 0;
        for (const Order* o = level ? level->head : nullptr; o; o = o->queue_next) {
            if (o->market_data_liquidity) {
                syn += o->remaining_quantity;
                continue;
            }
            if (syn) parts.push_back("syn " + qty_text(syn));
            syn = 0;
            parts.push_back(o->owner_id + " " + qty_text(o->remaining_quantity));
        }
        if (syn) parts.push_back("syn " + qty_text(syn));
        return t[1] + " " + t[2] + " " + t[3] + " = " +
               (parts.empty() ? "empty" : join(parts, 0, " | "));
    }

    const Golden& g_;
    Simulator& sim_;
    std::string model_;
    std::map<uint64_t, size_t> timers_;
    std::map<std::string, OrderId> ids_;
    std::map<OrderId, std::string> refs_;
};

std::string lines(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& l : v) s += "\n    " + l;
    return s.empty() ? "\n    (none)" : s;
}

void run_golden(const Golden& g) {
    TempFile f;
    g.data.write(f);
    for (const auto& model : g.queues) {
        CAPTURE(g.name);
        CAPTURE(model);
        SimulatorOptions opt;
        opt.to_ns = g.end_ns;
        opt.queue_model = model;
        Simulator sim(f.str(), opt);
        Runner runner(g, sim, model);
        SessionConfig sc = session("S1", g.latency_ms);
        sc.fees = g.fees;
        sim.add_strategy(runner, sc);
        sim.gateway().set_listener(&runner);
        sim.run();

        std::vector<std::string> want, want_queues;
        for (const auto& st : g.expects) {
            if (applies(st, model)) want.push_back(join(st.t, 1));
        }
        for (const auto& st : g.queue_at) {
            if (applies(st, model)) want_queues.push_back(join(st.t, 1));
        }
        CHECK_MESSAGE(runner.reports == want, "reports" << lines(runner.reports) << "\n  expected" << lines(want));
        CHECK_MESSAGE(runner.queues == want_queues,
                      "queues" << lines(runner.queues) << "\n  expected" << lines(want_queues));

        const Account& a = sim.gateway().exchange_account(0);
        const std::string got = "position " + qty_text(a.position) + " cash " + format_fixed(a.cash) + " fees " +
                                format_fixed(a.fees);
        for (const auto& st : g.finals) {
            if (!applies(st, model)) continue;
            // Compare as numbers: "0.2" and "0.20000000" are the same amount
            const std::string want_final = "position " + qty_text(fixed(g, st.line, st.t[2])) + " cash " +
                                           format_fixed(fixed(g, st.line, st.t[4])) + " fees " +
                                           format_fixed(fixed(g, st.line, st.t[6]));
            CHECK(got == want_final);
        }
    }
}

std::vector<fs::path> golden_files() {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(BLOCKSPRINT_GOLDEN_DIR)) {
        if (e.path().extension() == ".golden") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

TEST_SUITE("golden") {

TEST_CASE("every golden scenario file runs end to end and matches its hand-computed results") {
    const auto files = golden_files();
    CHECK(files.size() >= 8);
    for (const auto& path : files) {
        CAPTURE(path.filename().string());
        Golden g;
        REQUIRE_NOTHROW(g = parse(path));
        run_golden(g);
    }
}

TEST_CASE("golden file errors name the file and line") {
    TempPath p(".golden");
    std::ofstream(p.path) << "end 10\ndata 0 snapshot bid 100.001 1\n";
    CHECK_THROWS_WITH(parse(p.path), doctest::Contains(".golden:2: 100.001 is not a whole number"));
    std::ofstream(p.path) << "end 10\nfrobnicate 1\n";
    CHECK_THROWS_WITH(parse(p.path), doctest::Contains(".golden:2: unknown statement frobnicate"));
}

} // TEST_SUITE("golden")
