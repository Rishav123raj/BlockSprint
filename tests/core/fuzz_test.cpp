// Fuzz / property test (P1.16): seeded random operation sequences run through the engine and through a
// naive reference book, in lockstep. After every operation the return values, the trades, the full
// execution-report streams and the whole depth of every book must be identical, and the engine's book
// invariants must hold.
//
// The reference keeps every resting order in one flat vector and re-derives price-time priority by
// sorting on each use: O(n log n) per operation, but with none of the engine's maps, lists, iterators,
// lookup table or level totals, which is where bookkeeping bugs hide. It reuses only fee_for() (exact
// fee rounding, tested on its own in fees_test).
//
// Scale: 20 seeds x 5,000 operations per run (10^5 operations). Set BLOCKSPRINT_FUZZ_OPS to change the
// operations per seed (the nightly CI job runs 10^6 in total) and BLOCKSPRINT_FUZZ_SEED to replay one seed.
#include <doctest.h>
#include "test_helpers.hpp"
#include "workload.hpp"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------------
// Reference book
// ---------------------------------------------------------------------------------------------------

struct RefOrder {
    std::string id;
    std::string owner;
    StpPolicy policy = StpPolicy::CANCEL_MAKER;
    bool market_data = false;
    Side side = Side::BUY;
    OrderType type = OrderType::LIMIT;
    Price price = 0;
    Quantity quantity = 0;  // total, as last set
    Quantity filled = 0;
    Quantity remaining = 0;
    int64_t fees = 0;
    uint64_t arrival = 0;  // time priority: larger = later
};

// State shared by every book of one reference engine, like MatchingEngine's counters and fee book
struct RefShared {
    uint64_t trade_seq = 0;
    uint64_t report_seq = 0;
    uint64_t arrival_seq = 0;
    uint64_t now = 0;
    FeeSchedule default_fees;
    std::map<std::string, FeeSchedule> owner_fees;

    const FeeSchedule& fees_for(const std::string& owner) const {
        auto it = owner_fees.find(owner);
        return it == owner_fees.end() ? default_fees : it->second;
    }
};

class RefBook {
public:
    RefBook(std::string symbol, RefShared& shared) : symbol_(std::move(symbol)), sh_(shared) {}

    std::vector<Trade> submit(RefOrder o, std::vector<ExecReport>& out) {
        o.filled = 0;
        o.fees = 0;
        return execute(o, ExecType::ACK, out);
    }

    std::vector<Trade> replace(const RefOrder& o, std::vector<ExecReport>& out) {
        if (auto old = take(o.id)) {
            emit(out, ExecType::CANCELLED, *old, ExecReason::REPLACED);
        }
        return submit(o, out);
    }

    bool cancel(const std::string& id, std::vector<ExecReport>& out) {
        auto old = take(id);
        if (!old) {
            return false;
        }
        emit(out, ExecType::CANCELLED, *old, ExecReason::USER);
        return true;
    }

    bool amend(const std::string& id, Quantity new_total, std::optional<Price> new_price, std::vector<Trade>& trades,
               std::vector<ExecReport>& out) {
        const RefOrder* o = find(id);
        if (!o) {
            return false;
        }
        Quantity new_open = new_total - o->filled;
        if (new_open <= 0) {
            return cancel(id, out);
        }
        return modify(id, new_open, new_price.value_or(o->price), trades, out);
    }

    // new_open: the new open quantity (1.0 semantics)
    bool modify(const std::string& id, Quantity new_open, Price new_price, std::vector<Trade>& trades,
                std::vector<ExecReport>& out) {
        RefOrder* o = find(id);
        if (!o) {
            return false;
        }
        if (new_open <= 0) {
            return cancel(id, out);
        }
        RefOrder proposed = *o;
        proposed.price = new_price;
        proposed.quantity = o->filled + new_open;
        proposed.remaining = new_open;
        if (new_price <= 0) {  // resting orders are LIMIT or LIMIT_MAKER: priced
            emit(out, ExecType::REJECTED, proposed, ExecReason::INVALID_PRICE);
            return false;
        }
        if (o->type == OrderType::LIMIT_MAKER && crosses_book(proposed)) {
            emit(out, ExecType::REJECTED, proposed, ExecReason::POST_ONLY_WOULD_TAKE);
            return false;
        }
        if (o->price == new_price && new_open < o->remaining) {
            // Reduced at the same price: amended in place, keeps its time priority
            o->remaining = new_open;
            o->quantity = o->filled + new_open;
            emit(out, ExecType::AMENDED, *o);
            return true;
        }
        RefOrder moved = *take(id);
        moved.price = new_price;
        moved.quantity = moved.filled + new_open;
        trades = execute(moved, ExecType::AMENDED, out);
        return true;
    }

    // All levels, best first, as (price, total quantity)
    std::vector<std::pair<Price, Quantity>> levels(Side side) const {
        std::map<Price, Quantity> agg;
        for (const auto& o : resting_) {
            if (o.side == side) {
                agg[o.price] += o.remaining;
            }
        }
        std::vector<std::pair<Price, Quantity>> v(agg.begin(), agg.end());
        if (side == Side::BUY) {
            std::reverse(v.begin(), v.end());
        }
        return v;
    }

    size_t resting_count() const { return resting_.size(); }

    bool is_resting(const std::string& id) const { return find(id) != nullptr; }

private:
    static bool crosses(const RefOrder& o, Price level) { return o.side == Side::BUY ? o.price >= level : o.price <= level; }

    static bool self_trade(const RefOrder& taker, const RefOrder& maker) {
        return taker.policy != StpPolicy::NONE && !taker.owner.empty() && taker.owner == maker.owner;
    }

    // IDs of the resting orders that `taker` may meet, in price-time priority
    std::vector<std::string> queue_against(const RefOrder& taker) const {
        std::vector<const RefOrder*> v;
        for (const auto& o : resting_) {
            if (o.side != taker.side) {
                v.push_back(&o);
            }
        }
        std::sort(v.begin(), v.end(), [&](const RefOrder* a, const RefOrder* b) {
            if (a->price != b->price) {
                return taker.side == Side::BUY ? a->price < b->price : a->price > b->price;
            }
            return a->arrival < b->arrival;
        });
        std::vector<std::string> ids;
        for (const auto* o : v) {
            ids.push_back(o->id);
        }
        return ids;
    }

    bool crosses_book(const RefOrder& o) const {
        for (const auto& r : resting_) {
            if (r.side != o.side && crosses(o, r.price)) {
                return true;
            }
        }
        return false;
    }

    RefOrder* find(const std::string& id) {
        for (auto& o : resting_) {
            if (o.id == id) {
                return &o;
            }
        }
        return nullptr;
    }
    const RefOrder* find(const std::string& id) const { return const_cast<RefBook*>(this)->find(id); }

    std::optional<RefOrder> take(const std::string& id) {
        for (auto it = resting_.begin(); it != resting_.end(); ++it) {
            if (it->id == id) {
                RefOrder o = *it;
                resting_.erase(it);
                return o;
            }
        }
        return std::nullopt;
    }

    ExecReport& emit(std::vector<ExecReport>& out, ExecType type, const RefOrder& o, ExecReason reason = ExecReason::NONE) {
        ExecReport r;
        r.seq = ++sh_.report_seq;
        r.timestamp = sh_.now;
        r.type = type;
        r.reason = reason;
        r.order_id = o.id;
        r.owner_id = o.owner;
        r.symbol = symbol_;
        r.side = o.side;
        r.order_type = o.type;
        r.price = o.price;
        r.quantity = o.quantity;
        r.cum_qty = o.filled;
        r.leaves_qty = (type == ExecType::CANCELLED || type == ExecType::REJECTED) ? 0 : o.remaining;
        r.cum_fee = o.fees;
        out.push_back(r);
        return out.back();
    }

    // Sweeps the opposite side; returns true if self-trade prevention cancelled the taker
    bool sweep(RefOrder& t, bool enforce_limit, std::vector<Trade>& trades, std::vector<ExecReport>& out) {
        for (const std::string& maker_id : queue_against(t)) {
            if (t.remaining <= 0) {
                break;
            }
            RefOrder* m = find(maker_id);
            if (enforce_limit && !crosses(t, m->price)) {
                break;
            }
            if (self_trade(t, *m)) {
                if (t.policy == StpPolicy::SKIP) {
                    continue;
                }
                if (t.policy == StpPolicy::CANCEL_MAKER || t.policy == StpPolicy::CANCEL_BOTH) {
                    RefOrder gone = *take(maker_id);
                    emit(out, ExecType::CANCELLED, gone, ExecReason::STP_MAKER);
                }
                if (t.policy == StpPolicy::CANCEL_TAKER || t.policy == StpPolicy::CANCEL_BOTH) {
                    return true;
                }
                continue;
            }
            Quantity q = std::min(t.remaining, m->remaining);
            Trade tr{std::to_string(++sh_.trade_seq), symbol_, m->price, q, t.side, m->id, t.id, sh_.now};
            trades.push_back(tr);
            t.remaining -= q;
            t.filled += q;
            m->remaining -= q;
            m->filled += q;
            Price px = m->price;
            for (RefOrder* side : {&t, m}) {
                bool taker = side == &t;
                int64_t fee = 0;
                if (!side->market_data) {
                    const FeeSchedule& s = sh_.fees_for(side->owner);
                    fee = fee_for(px, q, taker ? s.taker_rate : s.maker_rate);
                }
                side->fees += fee;
                ExecReport& f = emit(out, ExecType::FILL, *side);
                f.fee = fee;
                f.last_px = px;
                f.last_qty = q;
                f.liquidity = taker ? Liquidity::TAKER : Liquidity::MAKER;
                f.trade_id = tr.trade_id;
            }
            if (m->remaining <= 0) {
                take(maker_id);
            }
        }
        return false;
    }

    bool fok_fillable(const RefOrder& t) const {
        Quantity acc = 0;
        for (const std::string& id : queue_against(t)) {
            const RefOrder* m = find(id);
            if (!crosses(t, m->price)) {
                break;
            }
            if (self_trade(t, *m)) {
                if (t.policy == StpPolicy::CANCEL_TAKER || t.policy == StpPolicy::CANCEL_BOTH) {
                    return false;
                }
                continue;
            }
            acc += m->remaining;
            if (acc >= t.remaining) {
                return true;
            }
        }
        return false;
    }

    // The common path of a new order (first = ACK) and a re-queued amend (first = AMENDED)
    std::vector<Trade> execute(RefOrder o, ExecType first, std::vector<ExecReport>& out) {
        o.remaining = o.quantity - o.filled;
        if (find(o.id)) {
            emit(out, ExecType::REJECTED, o, ExecReason::DUPLICATE_ORDER_ID);
            return {};
        }
        if (first == ExecType::ACK) {
            if (o.quantity <= 0) {
                emit(out, ExecType::REJECTED, o, ExecReason::INVALID_QUANTITY);
                return {};
            }
            if (o.type != OrderType::MARKET && o.price <= 0) {
                emit(out, ExecType::REJECTED, o, ExecReason::INVALID_PRICE);
                return {};
            }
            if (o.type == OrderType::LIMIT_MAKER && crosses_book(o)) {
                emit(out, ExecType::REJECTED, o, ExecReason::POST_ONLY_WOULD_TAKE);
                return {};
            }
        }
        emit(out, first, o);
        o.arrival = ++sh_.arrival_seq;

        std::vector<Trade> trades;
        bool taker_cancelled = false;
        ExecReason unfilled = ExecReason::NONE;
        switch (o.type) {
            case OrderType::LIMIT:
                taker_cancelled = sweep(o, true, trades, out);
                if (o.remaining > 0 && !taker_cancelled) {
                    if (crosses_book(o)) {
                        unfilled = ExecReason::WOULD_CROSS;
                    } else {
                        resting_.push_back(o);
                    }
                }
                break;
            case OrderType::LIMIT_MAKER:
                if (crosses_book(o)) {
                    unfilled = ExecReason::WOULD_CROSS;
                } else {
                    resting_.push_back(o);
                }
                break;
            case OrderType::MARKET:
                taker_cancelled = sweep(o, false, trades, out);
                unfilled = ExecReason::NO_LIQUIDITY;
                break;
            case OrderType::IOC:
                taker_cancelled = sweep(o, true, trades, out);
                unfilled = ExecReason::IOC_REMAINDER;
                break;
            case OrderType::FOK:
                if (fok_fillable(o)) {
                    taker_cancelled = sweep(o, true, trades, out);
                }
                unfilled = ExecReason::FOK_KILL;
                break;
        }
        if (o.remaining > 0 && (taker_cancelled || unfilled != ExecReason::NONE)) {
            emit(out, ExecType::CANCELLED, o, taker_cancelled ? ExecReason::STP_TAKER : unfilled);
        }
        return trades;
    }

    std::string symbol_;
    RefShared& sh_;
    std::vector<RefOrder> resting_;  // insertion order only; priority is derived on demand
};

// ---------------------------------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------------------------------

std::string describe(const ExecReport& r) {
    std::ostringstream s;
    s << "#" << r.seq << " t=" << r.timestamp << " " << execTypeToString(r.type) << "/" << execReasonToString(r.reason)
      << " " << r.symbol << " " << r.order_id << " owner=" << r.owner_id << " " << sideToString(r.side) << " "
      << orderTypeToString(r.order_type) << " px=" << format_fixed(r.price) << " qty=" << format_fixed(r.quantity)
      << " cum=" << format_fixed(r.cum_qty) << " leaves=" << format_fixed(r.leaves_qty)
      << " cum_fee=" << r.cum_fee << " last=" << format_fixed(r.last_qty) << "@" << format_fixed(r.last_px)
      << " liq=" << static_cast<int>(r.liquidity) << " trade=" << r.trade_id << " fee=" << r.fee;
    return s.str();
}

std::string describe(const Trade& t) {
    return t.trade_id + " " + t.symbol + " " + format_fixed(t.quantity) + "@" + format_fixed(t.price) + " " +
           sideToString(t.aggressor_side) + " maker=" + t.maker_order_id + " taker=" + t.taker_order_id +
           " t=" + std::to_string(t.timestamp);
}

// Every field except `text`, whose wording is the engine's own; REJECTED reports must still carry one
bool same(const ExecReport& a, const ExecReport& b) {
    return a.seq == b.seq && a.timestamp == b.timestamp && a.type == b.type && a.reason == b.reason &&
           a.order_id == b.order_id && a.owner_id == b.owner_id && a.symbol == b.symbol && a.side == b.side &&
           a.order_type == b.order_type && a.price == b.price && a.quantity == b.quantity && a.cum_qty == b.cum_qty &&
           a.leaves_qty == b.leaves_qty && a.cum_fee == b.cum_fee && a.last_px == b.last_px &&
           a.last_qty == b.last_qty && a.liquidity == b.liquidity && a.trade_id == b.trade_id && a.fee == b.fee &&
           (a.type != ExecType::REJECTED || !a.text.empty());
}

bool same(const Trade& a, const Trade& b) {
    return a.trade_id == b.trade_id && a.symbol == b.symbol && a.price == b.price && a.quantity == b.quantity &&
           a.aggressor_side == b.aggressor_side && a.maker_order_id == b.maker_order_id &&
           a.taker_order_id == b.taker_order_id && a.timestamp == b.timestamp;
}

// First difference between the engine's and the reference's results, or "" if they agree
template <typename T>
std::string diff(const char* what, const std::vector<T>& engine, const std::vector<T>& ref) {
    size_t n = std::max(engine.size(), ref.size());
    for (size_t i = 0; i < n; ++i) {
        bool both = i < engine.size() && i < ref.size();
        if (!both || !same(engine[i], ref[i])) {
            return std::string(what) + "[" + std::to_string(i) + "] engine: " +
                   (i < engine.size() ? describe(engine[i]) : "(none)") +
                   "\n    reference: " + (i < ref.size() ? describe(ref[i]) : "(none)");
        }
    }
    return "";
}

std::string diff_depth(const std::string& symbol, MatchingEngine& eng, const RefBook& ref) {
    Depth d = eng.depth(symbol, 1 << 20);
    for (Side side : {Side::BUY, Side::SELL}) {
        const auto& got = side == Side::BUY ? d.bids : d.asks;
        auto want = ref.levels(side);
        bool equal = got.size() == want.size();
        for (size_t i = 0; equal && i < got.size(); ++i) {
            equal = got[i].price == want[i].first && got[i].quantity == want[i].second;
        }
        if (!equal) {
            std::string s = symbol + " " + sideToString(side) + " depth differs\n    engine:   ";
            for (const auto& l : got) {
                s += format_fixed(l.quantity) + "@" + format_fixed(l.price) + " ";
            }
            s += "\n    reference: ";
            for (const auto& [p, q] : want) {
                s += format_fixed(q) + "@" + format_fixed(p) + " ";
            }
            return s;
        }
    }
    return "";
}

// ---------------------------------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------------------------------

const std::vector<std::string> kSymbols = {"FZ-A", "FZ-B"};
const std::string kNoBook = "FZ-NONE";  // never receives an order

struct FuzzStats {
    size_t ops = 0;
    size_t trades = 0;
    size_t reports = 0;
    std::map<std::string, size_t> report_kinds;  // "type/reason"
    size_t max_resting = 0;
};

// Runs `ops` random operations; returns "" or a description of the first divergence
std::string run_fuzz(uint64_t seed, int ops, FuzzStats& stats) {
    auto clock = std::make_shared<SimClock>(1700000000000000000ULL);
    MatchingEngine eng(clock);
    RefShared sh;
    sh.now = clock->now_ns();

    // Fees: a default schedule, a maker rebate for P1 and a fee-free P2, so rounding and signs are exercised
    sh.default_fees = FeeSchedule{20000, 100000};  // 0.02% / 0.10%
    sh.owner_fees["P1"] = FeeSchedule{-10000, 50000};
    sh.owner_fees["P2"] = FeeSchedule{0, 0};
    eng.set_default_fee_schedule(sh.default_fees);
    for (const auto& [owner, s] : sh.owner_fees) {
        eng.set_fee_schedule(owner, s);
    }

    std::map<std::string, RefBook> books;
    for (const auto& s : kSymbols) {
        books.emplace(s, RefBook(s, sh));
    }

    Lcg rng{seed};
    auto pick = [&](uint32_t n) { return rng.next() % n; };
    std::vector<std::pair<std::string, std::string>> ids;  // (symbol, order_id) of recent submissions
    int next_id = 0;

    // A tight price band (21 ticks of 0.01 around 100) so orders cross often; odd quantities make the
    // fees round. Rarely an invalid value, to exercise rejections.
    auto price = [&]() -> Price { return fx(100.0) + (static_cast<int64_t>(pick(21)) - 10) * 1000000; };
    auto qty = [&]() -> Quantity { return (1 + static_cast<int64_t>(pick(40))) * 3125000; };  // 0.03125 .. 1.25
    auto recent = [&]() -> std::pair<std::string, std::string> {
        if (ids.empty() || pick(20) == 0) {
            return {kSymbols[pick(2)], "UNKNOWN" + std::to_string(pick(5))};
        }
        size_t window = std::min<size_t>(ids.size(), 24);
        return ids[ids.size() - 1 - pick(static_cast<uint32_t>(window))];
    };

    for (int i = 0; i < ops; ++i) {
        clock->advance(1 + pick(3) * 1000);  // sometimes several reports share a timestamp
        sh.now = clock->now_ns();

        std::vector<ExecReport> got_r, want_r;
        std::vector<Trade> got_t, want_t;
        bool got_ok = true, want_ok = true;
        std::string op;

        uint32_t action = pick(100);
        if (action < 62) {
            // Submit (or, 8% of the time, replace)
            const std::string& sym = kSymbols[pick(2)];
            uint32_t t = pick(100);
            OrderType type = t < 45   ? OrderType::LIMIT
                             : t < 60 ? OrderType::LIMIT_MAKER
                             : t < 72 ? OrderType::MARKET
                             : t < 86 ? OrderType::IOC
                                      : OrderType::FOK;
            RefOrder o;
            o.side = pick(2) ? Side::BUY : Side::SELL;
            o.type = type;
            o.price = type == OrderType::MARKET ? 0 : price();
            o.quantity = qty();
            uint32_t who = pick(10);
            if (who < 2) {
                o.owner = "MARKET";  // mirrored exchange liquidity, as the connector sends it
                o.policy = StpPolicy::SKIP;
                o.market_data = true;
                o.quantity *= 4;
            } else if (who < 9) {
                o.owner = "P" + std::to_string(pick(3));
                o.policy = static_cast<StpPolicy>(pick(5));
            }  // else anonymous, default policy
            uint32_t oddity = pick(100);
            if (oddity == 0) {
                o.quantity = 0;
            } else if (oddity == 1) {
                o.price = 0;  // MARKET doesn't need one; every other type is rejected
            }
            bool duplicate = oddity == 2 && !ids.empty();
            o.id = duplicate ? recent().second : "F" + std::to_string(next_id++);
            bool replace = !duplicate && pick(100) < 8 && !ids.empty();
            if (replace) {
                auto target = recent();
                if (target.first == sym) {
                    o.id = target.second;
                }
            }

            auto order = std::make_shared<Order>();
            order->order_id = o.id;
            order->symbol = sym;
            order->order_type = o.type;
            order->side = o.side;
            order->price = o.price;
            order->quantity = o.quantity;
            order->owner_id = o.owner;
            order->stp_policy = o.policy;
            order->market_data_liquidity = o.market_data;
            order->timestamp = sh.now;

            op = std::string(replace ? "replace " : "submit ") + sym + " " + o.id + " " + orderTypeToString(type) +
                 " " + sideToString(o.side) + " " + format_fixed(o.quantity) + "@" + format_fixed(o.price) +
                 " owner=" + o.owner + " stp=" + stpPolicyToString(o.policy);
            if (replace) {
                got_t = eng.replace_order(order, &got_r);
                want_t = books.at(sym).replace(o, want_r);
            } else {
                got_t = eng.submit_order(order, &got_r);
                want_t = books.at(sym).submit(o, want_r);
            }
            ids.emplace_back(sym, o.id);
        } else if (action < 80) {
            auto [sym, id] = pick(30) == 0 ? std::make_pair(kNoBook, std::string("X")) : recent();
            op = "cancel " + sym + " " + id;
            got_ok = eng.cancel_order(sym, id, &got_r);
            want_ok = books.count(sym) ? books.at(sym).cancel(id, want_r) : false;
        } else if (action < 90) {
            auto [sym, id] = pick(30) == 0 ? std::make_pair(kNoBook, std::string("X")) : recent();
            Quantity q = pick(15) == 0 ? 0 : qty();
            Price px = pick(8) == 0 ? 0 : price();  // 0: rejected (invalid price)
            op = "modify " + sym + " " + id + " open=" + format_fixed(q) + " px=" + format_fixed(px);
            got_ok = eng.modify_order(sym, id, q, px, got_t, &got_r);
            want_ok = books.count(sym) ? books.at(sym).modify(id, q, px, want_t, want_r) : false;
        } else {
            auto [sym, id] = pick(30) == 0 ? std::make_pair(kNoBook, std::string("X")) : recent();
            Quantity total = pick(6) == 0 ? qty() / 4 : qty() * 2;  // small totals cancel partly filled orders
            std::optional<Price> px;
            if (pick(2) == 0) {
                px = price();
            }
            op = "amend " + sym + " " + id + " total=" + format_fixed(total) +
                 " px=" + (px ? format_fixed(*px) : std::string("keep"));
            got_ok = eng.amend_order(sym, id, total, px, got_t, &got_r);
            want_ok = books.count(sym) ? books.at(sym).amend(id, total, px, want_t, want_r) : false;
        }

        const std::string where = "seed " + std::to_string(seed) + ", op " + std::to_string(i) + ": " + op + "\n    ";
        if (got_ok != want_ok) {
            return where + "return value: engine " + std::to_string(got_ok) + ", reference " + std::to_string(want_ok);
        }
        if (auto d = diff("trade", got_t, want_t); !d.empty()) {
            return where + d;
        }
        if (auto d = diff("report", got_r, want_r); !d.empty()) {
            return where + d;
        }
        size_t resting = 0;
        for (const auto& s : kSymbols) {
            if (auto d = diff_depth(s, eng, books.at(s)); !d.empty()) {
                return where + d;
            }
            if (auto v = eng.check_invariants(s); !v.empty()) {
                return where + "invariant: " + v.front();
            }
            resting += books.at(s).resting_count();
        }

        stats.ops++;
        stats.trades += got_t.size();
        stats.reports += got_r.size();
        for (const auto& r : got_r) {
            stats.report_kinds[execTypeToString(r.type) + "/" + execReasonToString(r.reason)]++;
        }
        stats.max_resting = std::max(stats.max_resting, resting);
    }
    return "";
}

int env_int(const char* name, int fallback) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv is fine here: read once, single-threaded
#endif
    const char* v = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return v && *v ? std::atoi(v) : fallback;
}

} // namespace

TEST_SUITE("fuzz") {

TEST_CASE("engine and naive reference book agree on every report, trade and level (seeded random ops)") {
    const int ops = env_int("BLOCKSPRINT_FUZZ_OPS", 5000);
    const int only_seed = env_int("BLOCKSPRINT_FUZZ_SEED", 0);
    std::vector<uint64_t> seeds;
    if (only_seed) {
        seeds.push_back(static_cast<uint64_t>(only_seed));
    } else {
        for (uint64_t s = 1; s <= 20; ++s) {
            seeds.push_back(s * 7919);
        }
    }

    FuzzStats stats;
    for (uint64_t seed : seeds) {
        CAPTURE(seed);
        std::string failure = run_fuzz(seed, ops, stats);
        if (!failure.empty()) {
            FAIL_CHECK(failure);
            break;
        }
    }
    MESSAGE("fuzz: " << stats.ops << " ops, " << stats.trades << " trades, " << stats.reports
                     << " reports, max resting " << stats.max_resting);

    // The run must actually reach every outcome the engine can produce, or it proves little
    if (stats.ops == seeds.size() * static_cast<size_t>(ops) && ops >= 2000) {
        for (const char* kind : {"ack/", "fill/", "amended/", "cancelled/user", "cancelled/replaced",
                                 "cancelled/ioc_remainder", "cancelled/no_liquidity", "cancelled/fok_kill",
                                 "cancelled/would_cross", "cancelled/stp_taker", "cancelled/stp_maker",
                                 "rejected/duplicate_order_id", "rejected/invalid_quantity", "rejected/invalid_price",
                                 "rejected/post_only_would_take"}) {
            CAPTURE(kind);
            CHECK(stats.report_kinds[kind] > 0);
        }
    }
}

TEST_CASE("the reference handles a hand-checked sequence like the engine (sanity check of the reference)") {
    // Expected: t1 fills a1 and a3, skips its own a2 and cancels its rest (would cross); the FOK t2 is
    // killed (its only liquidity is its own); the market sell t3 fills b1. Only a2 is left.
    RefShared sh;
    RefBook ref("REF", sh);
    MatchingEngine eng(std::make_shared<SimClock>(0));
    auto submit_both = [&](const std::string& id, Side side, OrderType type, double px, double q,
                           const std::string& owner = "", StpPolicy stp = StpPolicy::CANCEL_MAKER) {
        RefOrder o;
        o.id = id;
        o.side = side;
        o.type = type;
        o.price = fx(px);
        o.quantity = fx(q);
        o.owner = owner;
        o.policy = stp;
        std::vector<ExecReport> rr, er;
        auto rt = ref.submit(o, rr);
        auto order = make_order(id, "REF", type, side, px, q);
        order->owner_id = owner;
        order->stp_policy = stp;
        auto et = eng.submit_order(order, &er);
        CHECK(diff("trade", et, rt).empty());
        CHECK(diff("report", er, rr) == "");
    };
    submit_both("a1", Side::SELL, OrderType::LIMIT, 101, 1);
    submit_both("a2", Side::SELL, OrderType::LIMIT, 101, 2, "X");
    submit_both("a3", Side::SELL, OrderType::LIMIT, 102, 1);
    submit_both("b1", Side::BUY, OrderType::LIMIT, 99, 1);
    submit_both("t1", Side::BUY, OrderType::LIMIT, 102, 5, "X", StpPolicy::SKIP);  // skips a2, leaves it crossed
    submit_both("t2", Side::BUY, OrderType::FOK, 102, 2, "X", StpPolicy::CANCEL_TAKER);
    submit_both("t3", Side::SELL, OrderType::MARKET, 0, 3);
    CHECK(diff_depth("REF", eng, ref).empty());
    CHECK(ref.levels(Side::SELL) == std::vector<std::pair<Price, Quantity>>{{fx(101), fx(2)}});
}

} // TEST_SUITE("fuzz")
