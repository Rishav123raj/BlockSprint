#pragma once
// Shared by the simulator-level tests: temporary .bse files, a record stream builder, and a strategy made of
// lambdas that logs its callbacks.
#include "simulator.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace simtest {

using namespace sim;
namespace fs = std::filesystem;

constexpr int64_t kLot = 100000;  // 0.001
constexpr uint64_t kMs = 1000000;
constexpr uint8_t BID = 0, ASK = 1;
constexpr uint8_t AGGR_BUY = 0;

struct TempPath {
    fs::path path;
    explicit TempPath(const std::string& suffix = ".bse") {
        std::random_device rd;
        path = fs::temp_directory_path() / ("bs_sim_t_" + std::to_string(rd()) + std::to_string(rd()) + suffix);
    }
    ~TempPath() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};
using TempFile = TempPath;

inline bse::Header test_header() {
    bse::Header h;
    h.symbol = "TESTUSDT";
    h.tick_size = 1000000;  // 0.01
    h.lot_size = kLot;
    h.price_decimals = 2;
    h.qty_decimals = 3;
    h.min_notional = 0;
    return h;
}

// Builds a record stream; times in ms unless noted
struct Stream {
    std::vector<bse::Record> r;

    static bse::Record rec(int64_t ts_ns, uint8_t kind, uint8_t side, int64_t px, int64_t qty, uint16_t flags = 0) {
        return bse::Record{ts_ns, ts_ns, px, qty, 1, kind, side, flags};
    }
    Stream& snapshot(int64_t ms, std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels) {
        const int64_t t = ms * static_cast<int64_t>(kMs);
        r.push_back(rec(t, bse::SNAP_BEGIN, 0, 0, 0, bse::FROM_SNAPSHOT));
        for (auto [side, px, qty] : levels) r.push_back(rec(t, bse::SNAP_LEVEL, side, px, qty, bse::FROM_SNAPSHOT));
        r.push_back(rec(t, bse::SNAP_END, 0, 0, 0, bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH));
        return *this;
    }
    Stream& depth_ns(int64_t t, std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels) {
        for (auto [side, px, qty] : levels) r.push_back(rec(t, bse::DELTA, side, px, qty));
        r.back().flags = bse::LAST_IN_BATCH;
        return *this;
    }
    Stream& depth(int64_t ms, std::vector<std::tuple<uint8_t, int64_t, int64_t>> levels) {
        return depth_ns(ms * static_cast<int64_t>(kMs), std::move(levels));
    }
    Stream& trade_ns(int64_t t, uint8_t aggressor, int64_t px, int64_t lots) {
        r.push_back(rec(t, bse::TRADE, aggressor, px, lots));
        return *this;
    }
    Stream& trade(int64_t ms, uint8_t aggressor, int64_t px, int64_t lots) {
        return trade_ns(ms * static_cast<int64_t>(kMs), aggressor, px, lots);
    }
    Stream& gap(int64_t ms) {
        r.push_back(rec(ms * static_cast<int64_t>(kMs), bse::GAP, 0, 0, 0, bse::LAST_IN_BATCH));
        return *this;
    }
    void write(const TempPath& f) const { bse::write_file(f.str(), test_header(), r); }
    // The same records under another symbol (P6.7: multi-symbol runs)
    void write(const TempPath& f, const std::string& symbol) const {
        bse::Header h = test_header();
        h.symbol = symbol;
        bse::write_file(f.str(), h, r);
    }
};

inline std::shared_ptr<const LatencyModel> constant_ms(double ms) { return std::make_shared<ConstantLatency>(ms * 1e6); }

inline SessionConfig session(const std::string& owner, double latency_ms) {
    SessionConfig c;
    c.owner = owner;
    c.feed = c.entry = c.ack = constant_ms(latency_ms);
    return c;
}

// A strategy assembled from lambdas, logging "<ms> <what>"
struct Script : Strategy {
    std::function<void(Script&, Context&)> start;
    std::function<void(Script&, Context&, const ObservedBook&)> book;
    std::function<void(Script&, Context&)> end;
    std::vector<std::string> log;

    std::string name() const override { return "script"; }
    void note(const Context& ctx, const std::string& what) { log.push_back(std::to_string(ctx.now() / kMs) + " " + what); }

    void on_start(Context& ctx) override {
        if (start) start(*this, ctx);
    }
    void on_book(Context& ctx, const ObservedBook& b) override {
        if (book) book(*this, ctx, b);
    }
    void on_trade(Context& ctx, const TradeEvent&) override { note(ctx, "trade"); }
    void on_exec(Context& ctx, const ExecReport& r) override { note(ctx, execTypeToString(r.type)); }
    void on_timer(Context& ctx, uint64_t id) override { note(ctx, "timer " + std::to_string(id)); }
    void on_end(Context& ctx) override {
        if (end) end(*this, ctx);
    }
};

} // namespace simtest
