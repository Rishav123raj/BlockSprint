// Engine-only micro-benchmark (P1.18): orders per second without networking or Python.
//
//   passive   rest orders on both sides without crossing, then cancel them all
//   matching  random limit / market / IOC orders and cancels around one price (trade-heavy)
//   mirror    replace 20 mirrored price levels with new quantities, like the Binance connector
//
// Each workload runs in two configurations:
//   sim   EngineOptions{thread_safe = false}, no callbacks (a single-threaded backtest)
//   live  thread_safe = true and every callback registered (what the API server does)
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

// Returns the number of engine operations performed
size_t passive(MatchingEngine& eng, size_t ops) {
    size_t n = ops / 2;
    Lcg rng{1};
    for (size_t i = 0; i < n; ++i) {
        Side side = i % 2 ? Side::BUY : Side::SELL;
        Price offset = static_cast<Price>(1 + rng.next() % 500) * kTick;
        eng.submit_order(order("P" + std::to_string(i), OrderType::LIMIT, side,
                               side == Side::BUY ? kMid - offset : kMid + offset,
                               static_cast<Quantity>(1 + rng.next() % 100) * kLot));
    }
    for (size_t i = 0; i < n; ++i) {
        eng.cancel_order("BENCH", "P" + std::to_string(i));
    }
    return 2 * n;
}

size_t matching(MatchingEngine& eng, size_t ops) {
    Lcg rng{2};
    for (size_t i = 0; i < ops; ++i) {
        uint32_t action = rng.next() % 100;
        Side side = rng.next() % 2 ? Side::BUY : Side::SELL;
        Price price = kMid + (static_cast<Price>(rng.next() % 41) - 20) * kTick;
        Quantity qty = static_cast<Quantity>(1 + rng.next() % 50) * kLot;
        if (action < 70) {
            eng.submit_order(order("M" + std::to_string(i), OrderType::LIMIT, side, price, qty));
        } else if (action < 80) {
            eng.submit_order(order("M" + std::to_string(i), OrderType::MARKET, side, 0, qty));
        } else if (action < 90) {
            eng.submit_order(order("M" + std::to_string(i), OrderType::IOC, side, price, qty));
        } else {
            eng.cancel_order("BENCH", "M" + std::to_string(i - 1 - rng.next() % std::min<size_t>(i + 1, 64)));
        }
    }
    return ops;
}

size_t mirror(MatchingEngine& eng, size_t ops) {
    Lcg rng{3};
    for (size_t i = 0; i < ops; ++i) {
        uint32_t level = rng.next() % 20;
        Side side = level < 10 ? Side::BUY : Side::SELL;
        Price price = side == Side::BUY ? kMid - (1 + level) * kTick : kMid + (level - 9) * kTick;
        auto o = order("L" + std::to_string(level), OrderType::LIMIT, side, price,
                       static_cast<Quantity>(1 + rng.next() % 1000) * kLot);
        o->owner_id = "MARKET";
        o->stp_policy = StpPolicy::SKIP;
        eng.replace_order(o);
    }
    return ops;
}

void register_all_callbacks(MatchingEngine& eng, size_t& sink) {
    eng.register_bbo_callback([&sink](const std::string&, const Bbo& bbo) { sink += bbo.bid_qty > 0; });
    eng.register_depth_callback([&sink](const std::string&, const Depth& d) { sink += d.bids.size(); });
    eng.register_trade_callback([&sink](const std::string&, const Trade& t) { sink += t.trade_id.size(); });
    eng.register_exec_report_callback([&sink](const ExecReport& r) { sink += r.seq & 1; });
}

}  // namespace

int main(int argc, char** argv) {
    size_t ops = argc > 1 ? static_cast<size_t>(std::strtoull(argv[1], nullptr, 10)) : 200000;
    const int repeats = 3;

    struct Workload {
        const char* name;
        std::function<size_t(MatchingEngine&, size_t)> run;
    };
    std::vector<Workload> workloads = {{"passive", passive}, {"matching", matching}, {"mirror", mirror}};

    std::printf("bench_core: %zu operations per workload, best of %d runs\n\n", ops, repeats);
    std::printf("%-10s %-6s %14s %10s\n", "workload", "config", "ops/s", "ns/op");
    size_t sink = 0;
    for (const auto& w : workloads) {
        for (bool live : {false, true}) {
            double best = 1e300;
            for (int r = 0; r < repeats; ++r) {
                MatchingEngine eng(std::make_shared<WallClock>(), EngineOptions{live});
                if (live) {
                    register_all_callbacks(eng, sink);
                }
                auto t0 = std::chrono::steady_clock::now();
                size_t done = w.run(eng, ops);
                double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                best = std::min(best, secs / static_cast<double>(done));
            }
            std::printf("%-10s %-6s %14.0f %10.0f\n", w.name, live ? "live" : "sim", 1.0 / best, best * 1e9);
        }
    }
    std::printf("\n(callback sink %zu)\n", sink % 10);
    return 0;
}
