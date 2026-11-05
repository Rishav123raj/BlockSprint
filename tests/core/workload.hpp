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

// Golden values for seed kGoldenSeed, 5,000 operations.
// - stp_percent = 0: no self-trade prevention involved, so 1.0 and 2.0 must agree. Pinned on the pre-P1.9
//   engine, whose results matched the original v1.0.0 sources on the full characterization suite.
// - stp_percent = 15: includes mirrored-liquidity orders. Pinned at P1.9, which changed self-trade
//   semantics on purpose (D7, D21). The 1.0 value was 11732550826495055626.
constexpr uint64_t kGoldenSeed = 20261101ULL;
constexpr uint64_t kGoldenHashNoStp = 6980882564197707180ULL;
constexpr uint64_t kGoldenHashStp = 937915414169173351ULL;

// Seeded random mix of every order type, cancels and modifies, including mirrored-liquidity orders
// (owner "MARKET", StpPolicy::SKIP, IDs prefixed BINANCE_ as the 1.0 connector named them).
// Prices are multiples of 0.5 and quantities multiples of 0.25: exact in binary floating point, so the hash
// is identical whether the engine stores doubles (1.0) or fixed-point integers (2.0).
// after_op (optional) runs after every operation, e.g. to check book invariants.
// stp_percent is the share of mirrored-liquidity orders, which never trade with each other.
// mixed_owners gives every other order one of three owners and a random STP policy, so all policies
// are exercised. Off by default: it draws extra random numbers, which would change the golden hashes.
// post_only_and_amend turns a quarter of the limit orders into post-only (LIMIT_MAKER) orders and half of
// the modifications into Binance-style amends (new total quantity, sometimes keeping the price). Also
// off by default for the same reason.
inline WorkloadStats run_random_workload(MatchingEngine& eng, const std::string& symbol, uint64_t seed,
                                         int ops, const std::function<void(int op)>& after_op = {},
                                         uint32_t stp_percent = 15, bool mixed_owners = false,
                                         bool post_only_and_amend = false) {
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
            bool mirrored = (rng.next() % 100) < stp_percent;
            std::string id = (mirrored ? "BINANCE_" : "O") + std::to_string(i);
            double px = price();
            double q = qty();
            if (post_only_and_amend && type == OrderType::LIMIT && rng.next() % 4 == 0) {
                type = OrderType::LIMIT_MAKER;
            }
            if (type == OrderType::MARKET) {
                px = 0.0;
            }
            ids.push_back(id);
            stats.hash = fnv1a(stats.hash, "S" + id);
            auto order = make_order(id, symbol, type, side, px, q);
            if (mirrored) {
                order->owner_id = "MARKET";
                order->stp_policy = StpPolicy::SKIP;
            } else if (mixed_owners) {
                order->owner_id = "P" + std::to_string(rng.next() % 3);
                order->stp_policy = static_cast<StpPolicy>(rng.next() % 5);
            }
            record(eng.submit_order(order));
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
            bool ok;
            if (post_only_and_amend && rng.next() % 2 == 0) {
                std::optional<Price> new_price;
                if (rng.next() % 3 != 0) {
                    new_price = fx(px);
                }
                ok = eng.amend_order(symbol, id, fx(q), new_price, trades);
            } else {
                ok = eng.modify_order(symbol, id, fx(q), fx(px), trades);
            }
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
