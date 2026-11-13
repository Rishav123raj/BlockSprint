#pragma once
// Synthesizer (P3.6): turns L2 market data into L3 operations on the engine's FIFO book, so that strategy
// orders get realistic queue positions and the engine's own matching produces every fill (architecture §8).
//
// Synthetic orders are owned by MARKET (STP skip, market_data_liquidity: no fees, exempt from symbol rules).
// Every data level's synthetic quantity is driven towards the data's ABSOLUTE quantity:
//
//   trade q at p      a tape IOC (owner MARKET_TAPE) of q at limit p from the aggressor side. It consumes the
//                     queue front first, including strategy orders that are ahead (or at better prices). The
//                     real trade consumed q of real (synthetic) liquidity at p, so whatever strategy orders
//                     took is then removed from the front of the synthetic queue at p as well.
//   depth / snapshot  per changed level: synthetic > data: the excess is an unexplained cancel and the
//                     QueueModel picks where it comes from; synthetic < data: the difference joins the back.
//                     Decreases are applied before increases, so the synthetic book never crosses itself.
//   gap               all synthetic orders are removed; until the next snapshot nothing is matched (trades
//                     are skipped) and depth events are ignored. Strategy orders stay.
//
// Netting trades against depth (plan: pending_trade_qty) falls out of the absolute target: a trade has
// already removed its quantity when the depth event that reflects it arrives, so that decrease is not
// applied twice; only the remainder (real cancels) goes to the QueueModel.
//
// Crossing fill: a synthetic order added at a price that crosses a resting strategy order trades with it
// (at the strategy order's price), as the real market would have; the remainder rests.
//
// Assumptions (architecture §19): strategy orders don't change what others do. A strategy's taker fill
// removes synthetic liquidity until the data next changes that level (impact half-life 0); with a half-life
// tau > 0 (P6.1, ImpactOverlay) the level is driven to the data less the decaying amount taken, so the
// liquidity comes back gradually. With permanent impact (P6.6) every data price is moved by shift() ticks;
// a new shift moves the whole synthetic book at once (the overlay's entries are dropped). Adjacent synthetic orders with no strategy order between them are merged,
// which no queue model can tell apart.
#include "impact.hpp"
#include "queue_models.hpp"
#include "l2_book.hpp"
#include "replayer.hpp"
#include "rng.hpp"

#include "matching_engine.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sim {

struct SynthesizerOptions {
    std::string symbol;
    int64_t tick_size = 0;  // 1e-8 units per price tick of the data
    int64_t lot_size = 0;   // 1e-8 units per quantity lot of the data
    std::string market_owner = "MARKET";
    std::string tape_owner = "MARKET_TAPE";
    uint64_t impact_half_life_ns = 0;  // P6.1: 0 = a taken level is set back to the data on its next change
};

// Quantities are in 1e-8 units
struct SynthStats {
    uint64_t depth_batches = 0;
    uint64_t snapshots = 0;
    uint64_t gaps = 0;
    uint64_t trades = 0;
    uint64_t trades_unsynced = 0;      // trades during a gap: not matched
    uint64_t depth_unsynced = 0;       // depth events during a gap: ignored
    int64_t added_qty = 0;             // synthetic quantity added at the back
    int64_t cancelled_qty = 0;         // unexplained decreases, placed by the QueueModel
    int64_t trade_qty = 0;             // traded quantity in the data (synced)
    int64_t tape_synthetic_qty = 0;    // synthetic quantity the tape IOCs consumed
    int64_t tape_strategy_qty = 0;     // strategy quantity the tape IOCs filled
    int64_t trade_front_qty = 0;       // removed from the queue front after a trade that filled strategy orders
    // Gate V4 (P3.18 follow-up): with no strategy, each data trade of q at price P should take exactly q of
    // synthetic liquidity at P. Per synced trade:
    uint64_t trades_exact = 0;         // took exactly q of synthetic liquidity, all of it at P
    int64_t tape_through_qty = 0;      // synthetic liquidity taken at better prices than P (stale levels)
    int64_t tape_short_qty = 0;        // q the tape couldn't fill at P or better (e.g. iceberg orders)
    uint64_t crossing_fills = 0;       // trades of synthetic orders with crossed strategy orders
    int64_t crossing_qty = 0;
    int64_t would_cross_qty = 0;       // synthetic quantity dropped because the data itself was crossed
    // Impact overlay (P6.1)
    int64_t impact_taken_qty = 0;      // synthetic liquidity strategy taker fills consumed (tau > 0 only)
    int64_t impact_held_qty = 0;       // held back from the data when levels changed, summed over changes
    uint64_t shifts = 0;               // permanent impact (P6.6): times the shift changed
};

class Synthesizer {
public:
    // The engine (with the symbol's spec already set), the model and the Rng must outlive the synthesizer
    Synthesizer(MatchingEngine& engine, const QueueModel& model, Rng& rng, SynthesizerOptions options);

    // Applies one batch at the engine's current time (the caller sets the SimClock first)
    void apply(const Batch& batch);

    // The trades a strategy order made as the taker (submit, replace or amend), at the engine's current
    // time: what it took from synthetic liquidity feeds the impact overlay. Without one, nothing to do.
    void strategy_trades(const std::vector<Trade>& trades);
    const ImpactOverlay& impact() const { return impact_; }

    // Permanent impact (P6.6): from now on every data price is moved by ticks. The synthetic book moves at
    // once: levels that no longer match go, the moved ones are added at the back (crossing strategy orders
    // trade with them, as on any data change).
    void set_shift(int64_t ticks);
    int64_t shift() const { return shift_; }

    // What the data says (ticks and lots, moved by shift()), as of the last batch; empty while not synced
    const L2Book& data() const { return data_; }
    // A snapshot has established the book and no gap has happened since
    bool synced() const { return synced_; }
    const SynthStats& stats() const { return stats_; }
    const SynthesizerOptions& options() const { return options_; }

    // Synthetic quantity resting at a price (1e-8 units)
    int64_t synthetic_qty(Side side, Price price);

    // Consistency with the data, for tests and debug runs. Always: no synthetic quantity at a price the data
    // doesn't have, and never more than the data. With exact (no strategy orders, no trades since each
    // level last changed): every data level's synthetic quantity equals the data. One message per violation.
    std::vector<std::string> check(bool exact);

    // True for an order the synthesizer created (synthetic liquidity or a tape IOC)
    static bool is_synthetic_id(const std::string& order_id) { return !order_id.empty() && order_id[0] == '~'; }

private:
    // A level a batch touched; its target is the data's quantity after the whole batch
    struct Change {
        uint8_t side;  // data side: 0 bid, 1 ask
        int64_t px;    // ticks
    };

    void apply_depth(const Batch& batch);
    void apply_snapshot(const Batch& batch);
    void apply_trade(const bse::Record& r);
    void apply_gap();
    // Applies changes_: decreases first, then increases. unique: no level appears twice in changes_, so
    // each level is read from the engine once (P3.18)
    void reconcile(bool unique);
    bool changes_unique();
    // What a changed level is driven to: the data's quantity, less what the impact overlay still holds back
    int64_t target_qty(const Change& c);

    // Reads a level's queue into slots_ / queue_; returns its synthetic quantity
    int64_t scan(Side side, Price price);
    // Removes up to `amount` of synthetic quantity at a level, chosen by the queue model or from the front;
    // returns the quantity removed
    int64_t remove(Side side, Price price, int64_t amount, bool from_front);
    // The same, on the level scan() just read (slots_ / queue_ are current)
    int64_t remove_scanned(Price price, int64_t synthetic, int64_t amount, bool from_front);
    // Adds synthetic quantity at the back of a level
    void add(Side side, Price price, int64_t amount);
    std::string next_id();

    MatchingEngine& engine_;
    const QueueModel& model_;
    Rng& rng_;
    SynthesizerOptions options_;
    L2Book data_;
    bool synced_ = false;
    uint64_t id_seq_ = 0;
    SynthStats stats_;
    ImpactOverlay impact_;

    // Scratch, reused across batches
    int64_t shift_ = 0;  // ticks every data price is moved by (P6.6)
    std::vector<std::pair<uint8_t, Level>> moved_;
    std::vector<Change> changes_;
    std::vector<int64_t> after_decreases_;  // per change: synthetic quantity after the decreases
    std::vector<int64_t> targets_;          // per change: target_qty, computed once
    std::vector<uint64_t> keys_;            // for changes_unique
    std::vector<QueueSlot> slots_;
    std::vector<const Order*> queue_;  // the orders behind slots_; valid until the next engine call
    std::vector<int64_t> take_;
    std::vector<Level> levels_;
    std::vector<Trade> trades_;
    std::shared_ptr<Order> tape_;  // the tape IOC, reused
};

} // namespace sim
