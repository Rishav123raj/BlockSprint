#pragma once
// Seeded random order workload shared by the characterization (golden hash) and invariant tests.
#include "test_helpers.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

inline uint64_t fnv1a(uint64_t h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// Portable PRNG (std:: distributions differ between standard libraries).
struct Lcg {
    uint64_t state;
    uint32_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 33);
    }
};

inline std::string trade_key(const Trade& t) {
    return std::to_string(to_double(t.price)) + "|" + std::to_string(to_double(t.quantity)) + "|" +
           sideToString(t.aggressor_side) + "|" +
           t.maker_order_id + "|" + t.taker_order_id + ";";
}

struct WorkloadStats {
    uint64_t hash = 14695981039346656037ULL;
    size_t trades = 0;
    size_t cancels_ok = 0;
    size_t modifies_ok = 0;
};

// Seeded random mix of every order type, cancels and modifies, including BINANCE_-prefixed (STP) orders.
// Prices are multiples of 0.5 and quantities multiples of 0.25: exact in binary floating point, so the hash
// is identical whether the engine stores doubles (1.0) or fixed-point integers (2.0).
// after_op (optional) runs after every operation, e.g. to check book invariants.
// stp_percent is the share of orders with a BINANCE_ id, i.e. subject to 1.0 self-trade prevention.
inline WorkloadStats run_random_workload(MatchingEngine& eng, const std::string& symbol, uint64_t seed,
                                         int ops, const std::function<void(int op)>& after_op = {},
                                         uint32_t stp_percent = 15) {
    Lcg rng{seed};
    WorkloadStats stats;
    std::vector<std::string> ids;

    auto price = [&] { return 100.0 + (static_cast<int>(rng.next() % 21) - 10) * 0.5; };
    auto qty = [&] { return (1 + static_cast<int>(rng.next() % 8)) * 0.25; };
    // Cancel/modify targets come from the 16 most recent orders, which are the ones most likely still resting
    auto recent_id = [&]() -> const std::string& {
        size_t window = ids.size() < 16 ? ids.size() : 16;
        return ids[ids.size() - 1 - rng.next() % window];
    };
    auto record = [&](const std::vector<Trade>& trades) {
        for (const auto& t : trades) {
            stats.hash = fnv1a(stats.hash, trade_key(t));
        }
        stats.trades += trades.size();
    };

    for (int i = 0; i < ops; ++i) {
        uint32_t action = rng.next() % 100;
        if (action < 72) {
            OrderType type = action < 45   ? OrderType::LIMIT
                             : action < 55 ? OrderType::MARKET
                             : action < 65 ? OrderType::IOC
                                           : OrderType::FOK;
            Side side = (rng.next() % 2) ? Side::BUY : Side::SELL;
            std::string id = ((rng.next() % 100) < stp_percent ? "BINANCE_" : "O") + std::to_string(i);
            double px = price();
            double q = qty();
            if (type == OrderType::MARKET) {
                px = 0.0;
            }
            ids.push_back(id);
            stats.hash = fnv1a(stats.hash, "S" + id);
            record(eng.submit_order(make_order(id, symbol, type, side, px, q)));
        } else if (!ids.empty() && action < 90) {
            const std::string& id = recent_id();
            bool ok = eng.cancel_order(symbol, id);
            stats.cancels_ok += ok;
            stats.hash = fnv1a(stats.hash, "C" + id + (ok ? "1" : "0"));
        } else if (!ids.empty()) {
            const std::string& id = recent_id();
            double q = qty();
            double px = price();
            std::vector<Trade> trades;
            bool ok = eng.modify_order(symbol, id, fx(q), fx(px), trades);
            stats.modifies_ok += ok;
            stats.hash = fnv1a(stats.hash, "M" + id + (ok ? "1" : "0"));
            record(trades);
        }
        if (after_op) {
            after_op(i);
        }
    }

    nlohmann::json d = eng.get_l2_depth(symbol, 1000);
    for (const auto& lvl : d["bids"]) {
        stats.hash = fnv1a(stats.hash, "B" + std::to_string(std::stod(lvl[0].get<std::string>())) + "|" +
                                           std::to_string(std::stod(lvl[1].get<std::string>())));
    }
    for (const auto& lvl : d["asks"]) {
        stats.hash = fnv1a(stats.hash, "A" + std::to_string(std::stod(lvl[0].get<std::string>())) + "|" +
                                           std::to_string(std::stod(lvl[1].get<std::string>())));
    }
    return stats;
}
