#include "live_feed.hpp"

#include "latency_models.hpp"
#include "types.hpp"

#include <stdexcept>

namespace sim {

using nlohmann::json;

namespace {

[[noreturn]] void bad(const std::string& what) { throw std::invalid_argument("market feed: " + what); }

// An exact decimal from a JSON string or number (as Binance sends them: "70000.01000000")
int64_t decimal(const json& v, const char* what) {
    std::string text;
    if (v.is_string()) {
        text = v.get<std::string>();
    } else if (v.is_number()) {
        text = v.dump();
    } else {
        bad(std::string(what) + ": expected a decimal string, got " + v.dump());
    }
    const auto parsed = parse_fixed(text);
    if (!parsed) bad(std::string(what) + ": not a non-negative decimal with at most 8 places: " + text);
    return *parsed;
}

const std::string& string_field(const json& m, const char* key) {
    auto it = m.find(key);
    if (it == m.end() || !it->is_string()) bad(std::string("'") + key + "' missing or not a string");
    return it->get_ref<const std::string&>();
}

} // namespace

LiveFeed::LiveFeed(MatchingEngine& engine, LiveFeedOptions options) : engine_(engine), options_(std::move(options)) {
    make_queue_model(options_.queue_model);  // validates the name now, not at the first message
}

void LiveFeed::apply(const json& message) {
    if (!message.is_array()) {
        apply_one(message);
        return;
    }
    for (size_t i = 0; i < message.size(); ++i) {
        try {
            apply_one(message[i]);
        } catch (const std::invalid_argument& e) {
            throw std::invalid_argument(std::string(e.what()) + " (message " + std::to_string(i) + ")");
        }
    }
}

LiveFeed::Feed& LiveFeed::feed(const std::string& symbol) {
    const auto spec = engine_.symbol_spec(symbol);
    if (!spec) bad("unknown symbol '" + symbol + "' (POST /api/symbols first)");
    auto it = feeds_.find(symbol);
    // A changed spec (another tick or lot) starts the symbol over: it needs a snapshot again
    if (it != feeds_.end() && (it->second.spec.tick_size != spec->tick_size || it->second.spec.lot_size != spec->lot_size)) {
        feeds_.erase(it);
        it = feeds_.end();
    }
    if (it == feeds_.end()) {
        if (spec->tick_size <= 0 || spec->lot_size <= 0) {
            bad("symbol '" + symbol + "' needs a tick size and a lot size");
        }
        Feed& f = feeds_[symbol];
        f.spec = *spec;
        f.model = make_queue_model(options_.queue_model);
        f.rng = stream_rng(options_.seed, "queue/" + symbol);
        SynthesizerOptions so{symbol, spec->tick_size, spec->lot_size};
        so.impact_half_life_ns = options_.impact_half_life_ns;
        f.synth = std::make_unique<Synthesizer>(engine_, *f.model, f.rng, so);
        return f;
    }
    return it->second;
}

int64_t LiveFeed::ticks(const Feed& f, const json& v, const char* what) const {
    const int64_t x = decimal(v, what);
    if (x <= 0 || x % f.spec.tick_size != 0) {
        bad(std::string(what) + " " + format_fixed(x) + " is not a positive multiple of the tick " +
            format_fixed(f.spec.tick_size));
    }
    return x / f.spec.tick_size;
}

int64_t LiveFeed::lots(const Feed& f, const json& v, const char* what) const {
    const int64_t x = decimal(v, what);
    if (x % f.spec.lot_size != 0) {
        bad(std::string(what) + " " + format_fixed(x) + " is not a multiple of the lot " + format_fixed(f.spec.lot_size));
    }
    return x / f.spec.lot_size;
}

void LiveFeed::levels(const Feed& f, const json& m, const char* key, uint8_t side, uint8_t kind, uint16_t flags,
                      int64_t ts_local, int64_t ts_exch) {
    auto it = m.find(key);
    if (it == m.end()) return;
    if (!it->is_array()) bad(std::string("'") + key + "' must be a list of [price, qty]");
    for (const json& level : *it) {
        if (!level.is_array() || level.size() != 2) bad(std::string("'") + key + "': each level is [price, qty]");
        records_.push_back(bse::Record{ts_local, ts_exch, ticks(f, level[0], "price"), lots(f, level[1], "qty"), 0,
                                       kind, side, flags});
    }
}

void LiveFeed::apply_one(const json& m) {
    if (!m.is_object()) bad("expected a JSON object, got " + m.dump());
    const std::string& type = string_field(m, "type");
    Feed& f = feed(string_field(m, "symbol"));
    const auto now = static_cast<int64_t>(engine_.clock().now_ns());
    int64_t ts_exch = now;
    if (auto ts = m.find("ts"); ts != m.end()) {
        if (!ts->is_number_integer()) bad("'ts' must be an integer (ns)");
        ts_exch = ts->get<int64_t>();
    }
    records_.clear();
    BatchKind kind;
    if (type == "snapshot") {
        kind = BatchKind::SNAPSHOT;
        records_.push_back(bse::Record{now, ts_exch, 0, 0, 0, bse::SNAP_BEGIN, 0, bse::FROM_SNAPSHOT});
        levels(f, m, "bids", bse::BID, bse::SNAP_LEVEL, bse::FROM_SNAPSHOT, now, ts_exch);
        levels(f, m, "asks", bse::ASK, bse::SNAP_LEVEL, bse::FROM_SNAPSHOT, now, ts_exch);
        records_.push_back(bse::Record{now, ts_exch, 0, 0, 0, bse::SNAP_END, 0,
                                       static_cast<uint16_t>(bse::FROM_SNAPSHOT | bse::LAST_IN_BATCH)});
    } else if (type == "depth") {
        kind = BatchKind::DEPTH;
        levels(f, m, "bids", bse::BID, bse::DELTA, 0, now, ts_exch);
        levels(f, m, "asks", bse::ASK, bse::DELTA, 0, now, ts_exch);
        if (records_.empty()) return;  // nothing changed
        records_.back().flags = bse::LAST_IN_BATCH;
    } else if (type == "trade") {
        kind = BatchKind::TRADE;
        const std::string& side = string_field(m, "side");
        if (side != "buy" && side != "sell") bad("trade side must be 'buy' or 'sell' (the aggressor), got '" + side + "'");
        if (!m.contains("price") || !m.contains("qty")) bad("a trade needs 'price' and 'qty'");
        const int64_t qty = lots(f, m["qty"], "qty");
        if (qty <= 0) bad("a trade's qty must be positive");
        records_.push_back(bse::Record{now, ts_exch, ticks(f, m["price"], "price"), qty, 0, bse::TRADE,
                                       side == "buy" ? bse::BID : bse::ASK, bse::LAST_IN_BATCH});
    } else if (type == "gap") {
        kind = BatchKind::GAP;
        records_.push_back(bse::Record{now, ts_exch, 0, 0, 0, bse::GAP, 0, bse::LAST_IN_BATCH});
        gaps_.push_back(f.spec.symbol);
    } else {
        bad("unknown type '" + type + "' (snapshot, depth, trade or gap)");
    }
    f.synth->apply(Batch{kind, records_.data(), records_.data() + records_.size(), 0});
    ++f.messages;
    f.last_ns = static_cast<uint64_t>(now);
}

void LiveFeed::client_trades(const std::vector<Trade>& trades) {
    if (options_.impact_half_life_ns == 0 || trades.empty()) return;
    auto it = feeds_.find(trades.front().symbol);
    if (it != feeds_.end()) it->second.synth->strategy_trades(trades);
}

const Synthesizer* LiveFeed::synthesizer(const std::string& symbol) const {
    auto it = feeds_.find(symbol);
    return it == feeds_.end() ? nullptr : it->second.synth.get();
}

std::vector<std::string> LiveFeed::take_gaps() {
    std::vector<std::string> out;
    out.swap(gaps_);
    return out;
}

json LiveFeed::stats() const {
    json symbols = json::object();
    for (const auto& [symbol, f] : feeds_) {
        const SynthStats& s = f.synth->stats();
        const uint64_t synced_trades = s.trades - s.trades_unsynced;
        json top = nullptr;
        if (f.synth->synced()) {
            const auto bid = f.synth->data().best_bid();
            const auto ask = f.synth->data().best_ask();
            top = json{{"bid", bid ? json(format_fixed(bid->px * f.spec.tick_size)) : json(nullptr)},
                       {"ask", ask ? json(format_fixed(ask->px * f.spec.tick_size)) : json(nullptr)}};
        }
        symbols[symbol] = json{
            {"synced", f.synth->synced()},
            {"messages", f.messages},
            {"last_ns", f.last_ns},
            {"snapshots", s.snapshots},
            {"depth_batches", s.depth_batches},
            {"depth_unsynced", s.depth_unsynced},
            {"gaps", s.gaps},
            {"trades", s.trades},
            {"trades_unsynced", s.trades_unsynced},
            // Fidelity: the share of synced trades whose tape IOC took exactly the trade's quantity of synthetic
            // liquidity at its price (client orders ahead in the queue lower it: that's their passive fills)
            {"trade_reproduced", synced_trades ? json(static_cast<double>(s.trades_exact) / static_cast<double>(synced_trades))
                                               : json(nullptr)},
            {"trade_diverted_qty", format_fixed(s.tape_strategy_qty)},
            {"trade_through_qty", format_fixed(s.tape_through_qty)},
            {"trade_short_qty", format_fixed(s.tape_short_qty)},
            {"crossing_fills", s.crossing_fills},
            {"would_cross_qty", format_fixed(s.would_cross_qty)},
            {"impact_taken_qty", format_fixed(s.impact_taken_qty)},
            {"top", top},
        };
    }
    return json{{"queue_model", options_.queue_model}, {"seed", options_.seed},
                {"impact_half_life_ms", options_.impact_half_life_ns / 1e6}, {"symbols", symbols}};
}

} // namespace sim
