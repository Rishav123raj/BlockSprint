#include "simulator.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace sim {

namespace {

constexpr uint64_t kOpen = std::numeric_limits<uint64_t>::max();

uint64_t ts_of(const Batch& b) { return b.ts_local() > 0 ? static_cast<uint64_t>(b.ts_local()) : 0; }

} // namespace

Simulator::Simulator(const std::string& bse_path, SimulatorOptions options)
    : options_(std::move(options)), replayer_(bse_path), clock_(std::make_shared<SimClock>(0)),
      queue_rng_(stream_rng(options_.seed, "queue")), queue_(options_.tie_break) {
    if (options_.to_ns && options_.to_ns <= options_.from_ns) {
        throw std::invalid_argument("simulator: the window's end must be after its start");
    }
    if (options_.to_ns && options_.from_ns && options_.warmup_ns >= options_.to_ns - options_.from_ns) {
        throw std::invalid_argument("simulator: the warm-up covers the whole window");
    }
    const auto& h = replayer_.header();
    engine_ = std::make_unique<MatchingEngine>(clock_, EngineOptions{false, false});
    SymbolSpec spec;
    spec.symbol = h.symbol;
    spec.tick_size = h.tick_size;
    spec.lot_size = h.lot_size;
    spec.min_qty = h.lot_size;
    spec.min_notional = h.min_notional;
    engine_->set_symbol_spec(spec);
    model_ = make_queue_model(options_.queue_model);
    SynthesizerOptions so{h.symbol, h.tick_size, h.lot_size};
    so.impact_half_life_ns = options_.impact_half_life_ns;
    synth_ = std::make_unique<Synthesizer>(*engine_, *model_, queue_rng_, so);
    gateway_ = std::make_unique<SimGateway>(*engine_, queue_, h.symbol, h.tick_size, h.lot_size, options_.seed);
    gateway_->set_latency_regime(options_.latency_regime);
    if (options_.impact_half_life_ns) {
        gateway_->set_taker_trades([this](const std::vector<Trade>& trades) { synth_->strategy_trades(trades); });
    }
}

void Simulator::set_sampler(uint64_t every_ns, std::function<void(uint64_t)> fn) {
    if (every_ns == 0 && fn) {
        throw std::invalid_argument("simulator: the sampling interval must be positive");
    }
    sample_every_ = every_ns;
    sampler_ = std::move(fn);
}

void Simulator::sample_until(uint64_t t) {
    if (!sampler_) {
        return;
    }
    while (next_sample_ <= t) {
        clock_->set(next_sample_);
        sampler_(next_sample_);
        ++stats_.samples;
        next_sample_ += sample_every_;
    }
}

void Simulator::run() {
    if (ran_) {
        throw std::logic_error("Simulator::run: a simulator runs once");
    }
    ran_ = true;

    uint64_t data_time = 0;  // time of the last data batch; data never moves the clock backwards
    Batch b{};
    bool have = replayer_.next(b);
    auto batch_time = [&](const Batch& x) { return std::max(data_time, ts_of(x)); };

    // Fast-forward to the window's start
    const uint64_t start = options_.from_ns ? options_.from_ns : (have ? batch_time(b) : 0);
    while (have && batch_time(b) < start) {
        data_time = batch_time(b);
        clock_->set(data_time);
        synth_->apply(b);
        gateway_->observe(b);
        ++stats_.fast_forward_batches;
        have = replayer_.next(b);
    }
    clock_->set(std::max(clock_->now_ns(), start));

    uint64_t end = options_.to_ns ? options_.to_ns : kOpen;
    stats_.start_ns = start;
    stats_.trading_ns = start + options_.warmup_ns;
    gateway_->set_trading_from(stats_.trading_ns);
    next_sample_ = start;

    if (have) {
        queue_.push(batch_time(b), EventClass::MARKET_DATA, SimEvent{0, b});
    } else if (end == kOpen) {
        end = start + 1;
    }
    gateway_->start();

    while (!queue_.empty() && queue_.top().time < end) {
        sample_until(queue_.top().time);
        auto e = queue_.pop();
        clock_->set(e.time);
        ++stats_.events[static_cast<size_t>(e.cls)];
        if (e.cls != EventClass::MARKET_DATA) {
            gateway_->dispatch(e);
            continue;
        }
        data_time = e.time;
        const Batch& batch = std::get<Batch>(e.payload.body);
        synth_->apply(batch);
        gateway_->publish(batch);
        ++stats_.data_batches;
        if (replayer_.next(b)) {
            queue_.push(batch_time(b), EventClass::MARKET_DATA, SimEvent{0, b});
        } else if (end == kOpen) {
            end = data_time + 1;  // the strategies' events at the last data time still run
        }
    }
    if (end == kOpen) {
        end = clock_->now_ns() + 1;  // only reachable if the queue emptied with data left, which can't happen
    }

    while (!queue_.empty()) {
        if (queue_.top().cls == EventClass::MARKET_DATA) {
            stats_.data_left = 1;
        } else {
            ++stats_.dropped;
        }
        queue_.pop();
    }
    if (sampler_ && end > 0) {
        sample_until(end - 1);
    }
    clock_->set(std::max(clock_->now_ns(), end));
    stats_.end_ns = end;
    gateway_->end();
}

} // namespace sim
