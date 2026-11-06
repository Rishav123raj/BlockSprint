// Engine-only micro-benchmark (P1.18): orders per second without networking or Python.
//
//   passive   rest orders on both sides without crossing, then cancel them all
//   matching  random limit / market / IOC orders and cancels around one price (trade-heavy)
//   mirror    replace 20 mirrored price levels with new quantities, like the Binance connector
//
// Each workload runs in two configurations:
//   sim   EngineOptions{thread_safe = false}, SimClock, no callbacks (a single-threaded backtest)
//   live  thread_safe = true, WallClock and every callback registered (what the API server does)
//
// Two rates are printed per row:
//   engine     the engine calls alone. Orders (heap-allocated, with string IDs) are built in batches of
//              1,024 operations outside the timer, then the batch is run: like a caller that builds each
//              order just before submitting it, the orders are still in cache.
//   +orders    the same plus building each order, as a caller of this API pays it (the definition used
//              before P1.18)
//
// Usage: bench_core [operations per workload, default 200000]
#include "matching_engine.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

struct Lcg {
    uint64_t state;
    uint32_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 33);
    }
};

constexpr Price kTick = 1000000;            // 0.01
constexpr Price kMid = 100 * kFixedScale;   // 100.00
constexpr Quantity kLot = 1000000;          // 0.01

// One prepared engine call
struct Op {
    enum Kind { SUBMIT, REPLACE, CANCEL } kind = SUBMIT;
    std::shared_ptr<Order> order;  // SUBMIT / REPLACE
    std::string cancel_id;         // CANCEL
};

std::shared_ptr<Order> order(const std::string& id, OrderType type, Side side, Price price, Quantity qty) {
    auto o = std::make_shared<Order>();
    o->order_id = id;
    o->symbol = "BENCH";
    o->order_type = type;
    o->side = side;
    o->price = price;
    o->quantity = qty;
    return o;
}

std::vector<Op> passive(size_t ops) {
    size_t n = ops / 2;
    Lcg rng{1};
    std::vector<Op> script;
    script.reserve(2 * n);
    for (size_t i = 0; i < n; ++i) {
        Side side = i % 2 ? Side::BUY : Side::SELL;
        Price offset = static_cast<Price>(1 + rng.next() % 500) * kTick;
        script.push_back({Op::SUBMIT,
                          order("P" + std::to_string(i), OrderType::LIMIT, side,
                                side == Side::BUY ? kMid - offset : kMid + offset,
                                static_cast<Quantity>(1 + rng.next() % 100) * kLot),
                          {}});
    }
    for (size_t i = 0; i < n; ++i) {
        script.push_back({Op::CANCEL, nullptr, "P" + std::to_string(i)});
    }
    return script;
}

std::vector<Op> matching(size_t ops) {
    Lcg rng{2};
    std::vector<Op> script;
    script.reserve(ops);
    for (size_t i = 0; i < ops; ++i) {
        uint32_t action = rng.next() % 100;
        Side side = rng.next() % 2 ? Side::BUY : Side::SELL;
        Price price = kMid + (static_cast<Price>(rng.next() % 41) - 20) * kTick;
        Quantity qty = static_cast<Quantity>(1 + rng.next() % 50) * kLot;
        std::string id = "M" + std::to_string(i);
        if (action < 70) {
            script.push_back({Op::SUBMIT, order(id, OrderType::LIMIT, side, price, qty), {}});
        } else if (action < 80) {
            script.push_back({Op::SUBMIT, order(id, OrderType::MARKET, side, 0, qty), {}});
        } else if (action < 90) {
            script.push_back({Op::SUBMIT, order(id, OrderType::IOC, side, price, qty), {}});
        } else {
            script.push_back(
                {Op::CANCEL, nullptr, "M" + std::to_string(i - 1 - rng.next() % std::min<size_t>(i + 1, 64))});
        }
    }
    return script;
}

std::vector<Op> mirror(size_t ops) {
    Lcg rng{3};
    std::vector<Op> script;
    script.reserve(ops);
    for (size_t i = 0; i < ops; ++i) {
        uint32_t level = rng.next() % 20;
        Side side = level < 10 ? Side::BUY : Side::SELL;
        Price price = side == Side::BUY ? kMid - (1 + level) * kTick : kMid + (level - 9) * kTick;
        auto o = order("L" + std::to_string(level), OrderType::LIMIT, side, price,
                       static_cast<Quantity>(1 + rng.next() % 1000) * kLot);
        o->owner_id = "MARKET";
        o->stp_policy = StpPolicy::SKIP;
        script.push_back({Op::REPLACE, std::move(o), {}});
    }
    return script;
}

void run(MatchingEngine& eng, const std::vector<Op>& script, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
        const Op& op = script[i];
        switch (op.kind) {
            case Op::SUBMIT: eng.submit_order(op.order); break;
            case Op::REPLACE: eng.replace_order(op.order); break;
            case Op::CANCEL: eng.cancel_order("BENCH", op.cancel_id); break;
        }
    }
}

void register_all_callbacks(MatchingEngine& eng, size_t& sink) {
    eng.register_bbo_callback([&sink](const std::string&, const Bbo& bbo) { sink += bbo.bid_qty > 0; });
    eng.register_depth_callback([&sink](const std::string&, const Depth& d) { sink += d.bids.size(); });
    eng.register_trade_callback([&sink](const std::string&, const Trade& t) { sink += t.trade_id.size(); });
    eng.register_exec_report_callback([&sink](const ExecReport& r) { sink += r.seq & 1; });
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    size_t ops = argc > 1 ? static_cast<size_t>(std::strtoull(argv[1], nullptr, 10)) : 200000;
    const int repeats = 3;

    struct Workload {
        const char* name;
        std::function<std::vector<Op>(size_t)> build;
    };
    std::vector<Workload> workloads = {{"passive", passive}, {"matching", matching}, {"mirror", mirror}};

    std::printf("bench_core: %zu operations per workload, best of %d runs\n\n", ops, repeats);
    std::printf("%-10s %-6s %14s %10s %14s\n", "workload", "config", "engine ops/s", "ns/op", "+orders ops/s");
    size_t sink = 0;
    for (const auto& w : workloads) {
        for (bool live : {false, true}) {
            double best_run = 1e300;
            double best_total = 1e300;
            for (int r = 0; r < repeats; ++r) {
                std::shared_ptr<const IClock> clock;
                if (live) {
                    clock = std::make_shared<WallClock>();
                } else {
                    clock = std::make_shared<SimClock>(1793836800000000000ULL);
                }
                MatchingEngine eng(clock, EngineOptions{live});
                if (live) {
                    register_all_callbacks(eng, sink);
                }
                // The whole script is generated up front (the RNG sequence must not depend on batching);
                // its orders are then rebuilt batch by batch just before they run
                std::vector<Op> script = w.build(ops);
                double build = 0;
                double execute = 0;
                constexpr size_t kBatch = 1024;
                for (size_t b = 0; b < script.size(); b += kBatch) {
                    size_t e = std::min(script.size(), b + kBatch);
                    auto t0 = std::chrono::steady_clock::now();
                    for (size_t i = b; i < e; ++i) {
                        if (script[i].order) {
                            script[i].order = std::make_shared<Order>(*script[i].order);
                        }
                    }
                    build += seconds_since(t0);
                    auto t1 = std::chrono::steady_clock::now();
                    run(eng, script, b, e);
                    execute += seconds_since(t1);
                }
                double n = static_cast<double>(script.size());
                best_run = std::min(best_run, execute / n);
                best_total = std::min(best_total, (build + execute) / n);
            }
            std::printf("%-10s %-6s %14.0f %10.0f %14.0f\n", w.name, live ? "live" : "sim", 1.0 / best_run,
                        best_run * 1e9, 1.0 / best_total);
        }
    }
    std::printf("\n(callback sink %zu)\n", sink % 10);
    return 0;
}
