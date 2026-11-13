#pragma once
// Simulator (P3.11, architecture §5.3): the event loop over one .bse file, the synthesizer, the engine and
// the gateway.
//
//   fast-forward  data before the window's start goes to the synthesizer (the engine's book) and straight
//                 into the strategies' observed books, with no latency and no callbacks: the run starts with
//                 a book that is already in sync
//   window        on_start at the start; then data batches (one in the queue at a time, at their ts_local)
//                 and the gateway's events, ordered by (time, class, seq). Data stops at the window's end;
//                 strategy events at or after it are dropped. on_end at the end.
//   warm-up       the first warmup_ns of the window: strategies see the market but may not trade
//                 (ctx.can_trade() is false and requests throw)
//
// Data timestamps that go backwards (rare in the recorder's ts_local) are clamped to the last data time, so
// simulation time never decreases.
#include "event_queue.hpp"
#include "gateway.hpp"
#include "queue_models.hpp"
#include "replayer.hpp"
#include "rng.hpp"
#include "synthesizer.hpp"

#include "clock.hpp"
#include "matching_engine.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sim {

struct SimulatorOptions {
    uint64_t from_ns = 0;    // window start (ns since the epoch); 0 = the first data batch
    uint64_t to_ns = 0;      // window end, exclusive; 0 = just after the last data batch
    uint64_t warmup_ns = 0;  // no trading in [from, from + warmup)
    TieBreak tie_break = TieBreak::DATA_FIRST;
    std::string queue_model = "cancel_from_back";
    uint64_t impact_half_life_ns = 0;  // P6.1: how long a strategy's taker fills keep liquidity out (0: until
                                       // the data next changes the level)
    PermanentImpactConfig permanent_impact{};  // P6.6: square-root mid shift from strategy takes (off: coefficient 0)
    LatencyRegimeConfig latency_regime{};  // P6.3: delays scaled by the market's message rate (off: no curve)
    uint64_t seed = 0;       // every random stream of the run derives from it
};

struct SimStats {
    uint64_t start_ns = 0;          // window start: on_start
    uint64_t trading_ns = 0;        // end of the warm-up
    uint64_t end_ns = 0;            // window end: on_end
    uint64_t fast_forward_batches = 0;
    uint64_t data_batches = 0;      // in the window
    uint64_t data_left = 0;         // 1 if data remained after the window's end (not counted further)
    std::array<uint64_t, 5> events{};  // processed, by EventClass
    uint64_t dropped = 0;           // strategy events at or after the end
    uint64_t samples = 0;
};

class Simulator {
public:
    // Opens the file and builds the engine (symbol spec from the file's header), the queue model and the
    // synthesizer. Throws std::invalid_argument on a bad window or queue model.
    Simulator(const std::string& bse_path, SimulatorOptions options);
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    // Strategies must outlive the simulator
    uint32_t add_strategy(Strategy& strategy, SessionConfig config) { return gateway_->add_session(strategy, config); }

    // Calls fn(t) at start, start + every, ... (before the events at that time), e.g. for BBO and position
    // samples (P3.13). The engine's clock reads t during the call.
    void set_sampler(uint64_t every_ns, std::function<void(uint64_t t)> fn);

    // Runs the whole window; once per Simulator
    void run();

    const SimStats& stats() const { return stats_; }
    const bse::Header& header() const { return replayer_.header(); }
    const SimulatorOptions& options() const { return options_; }
    MatchingEngine& engine() { return *engine_; }
    SimGateway& gateway() { return *gateway_; }
    const Synthesizer& synthesizer() const { return *synth_; }
    const PermanentImpact& permanent_impact() const { return *permanent_; }
    uint64_t now() const { return clock_->now_ns(); }

private:
    void sample_until(uint64_t t);  // fires the samples due at or before t
    void strategy_trades(const std::vector<Trade>& trades);  // impact overlay and permanent impact

    SimulatorOptions options_;
    Replayer replayer_;
    std::shared_ptr<SimClock> clock_;
    std::unique_ptr<MatchingEngine> engine_;
    std::unique_ptr<QueueModel> model_;
    Rng queue_rng_;
    std::unique_ptr<Synthesizer> synth_;
    std::unique_ptr<PermanentImpact> permanent_;
    SimQueue queue_;
    std::unique_ptr<SimGateway> gateway_;

    uint64_t sample_every_ = 0;
    uint64_t next_sample_ = 0;
    std::function<void(uint64_t)> sampler_;

    bool ran_ = false;
    SimStats stats_;
};

} // namespace sim
