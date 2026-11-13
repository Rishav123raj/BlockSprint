#pragma once
// LiveFeed (P7.1): the live server's market data, through the simulator's own Synthesizer. A connector sends
// normalized L2 events (snapshot, depth delta, trade, gap) for the symbols the server knows; each becomes the
// same records the replayer reads from a .bse file and goes through the same Synthesizer, on the engine's
// clock (the wall clock in the server). So paper trading and the simulator share one L2 -> L3 path: synthetic
// FIFO queues, unexplained cancels placed by the queue model, trades as tape IOCs that reach a client's order
// when it is ahead in the queue.
//
// Messages (JSON objects; a frame may also hold an array of them). Prices and quantities are decimal strings
// (or numbers), on the symbol's tick and lot grid; `ts` (optional) is the exchange's time in ns.
//   {"type": "snapshot", "symbol": S, "bids": [[price, qty], ...], "asks": [...]}   the whole book
//   {"type": "depth",    "symbol": S, "bids": [[price, qty], ...], "asks": [...]}   absolute quantities; 0 removes
//   {"type": "trade",    "symbol": S, "price": P, "qty": Q, "side": "buy"|"sell"}   side: the aggressor's
//   {"type": "gap",      "symbol": S}                                              updates were lost
// Until a symbol's first snapshot (and after a gap until the next one) depth events are ignored and trades
// aren't matched, as in a replay. Ordering and gap detection are the connector's (P7.2).
#include "queue_models.hpp"
#include "replayer.hpp"
#include "rng.hpp"
#include "synthesizer.hpp"

#include "matching_engine.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sim {

struct LiveFeedOptions {
    std::string queue_model = "cancel_from_back";
    uint64_t seed = 0;                 // per symbol, the queue model's stream "queue/<SYMBOL>"
    uint64_t impact_half_life_ns = 0;  // P6.1; 0: a level taken by a client is set back to the data on its next change
};

class LiveFeed {
public:
    // The engine (with the symbols' specs set before their first message) must outlive the feed. Throws
    // std::invalid_argument for an unknown queue model.
    LiveFeed(MatchingEngine& engine, LiveFeedOptions options = {});

    // Applies one message, or an array of them, in order. Throws std::invalid_argument (with the message's
    // index in an array) on a malformed message, an unknown symbol, or a price or quantity off the grid; the
    // messages before it have been applied.
    void apply(const nlohmann::json& message);

    // Trades a client order made as the taker, for the impact overlay (nothing to do without one)
    void client_trades(const std::vector<Trade>& trades);

    // Per symbol: what the feed has applied and how faithfully (docs/live_platform_architecture.md 7.8)
    nlohmann::json stats() const;

    // Forgets every symbol (after an engine reset): each needs a snapshot again
    void reset() { feeds_.clear(); }

    const Synthesizer* synthesizer(const std::string& symbol) const;
    const LiveFeedOptions& options() const { return options_; }

    // Gap events applied since the last call, by symbol (for the server to tell market-data subscribers)
    std::vector<std::string> take_gaps();

private:
    struct Feed {
        SymbolSpec spec;
        std::unique_ptr<QueueModel> model;
        Rng rng{0};
        std::unique_ptr<Synthesizer> synth;
        uint64_t messages = 0;
        uint64_t last_ns = 0;  // engine time of the last message
    };

    void apply_one(const nlohmann::json& m);
    Feed& feed(const std::string& symbol);
    int64_t ticks(const Feed& f, const nlohmann::json& v, const char* what) const;
    int64_t lots(const Feed& f, const nlohmann::json& v, const char* what) const;
    void levels(const Feed& f, const nlohmann::json& m, const char* key, uint8_t side, uint8_t kind, uint16_t flags,
                int64_t ts_local, int64_t ts_exch);

    MatchingEngine& engine_;
    LiveFeedOptions options_;
    std::map<std::string, Feed> feeds_;
    std::vector<bse::Record> records_;  // the batch being built
    std::vector<std::string> gaps_;
};

} // namespace sim
