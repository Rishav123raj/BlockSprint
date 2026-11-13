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
    : Simulator(std::vector<std::string>{bse_path}, std::move(options)) {}

Simulator::Simulator(const std::vector<std::string>& bse_paths, SimulatorOptions options)
    : options_(std::move(options)), clock_(std::make_shared<SimClock>(0)), queue_(options_.tie_break) {
    if (options_.to_ns && options_.to_ns <= options_.from_ns) {
        throw std::invalid_argument("simulator: the window's end must be after its start");
    }
    if (options_.to_ns && options_.from_ns && options_.warmup_ns >= options_.to_ns - options_.from_ns) {
        throw std::invalid_argument("simulator: the warm-up covers the whole window");
    }
    if (bse_paths.empty()) {
        throw std::invalid_argument("simulator: no data file");
    }
    engine_ = std::make_unique<MatchingEngine>(clock_, EngineOptions{false, false});
    model_ = make_queue_model(options_.queue_model);
    std::vector<SymbolInfo> symbols;
    for (const std::string& path : bse_paths) {
        // The primary symbol's queue stream keeps its single-symbol name, so one-symbol runs don't change
        const std::string stream = feeds_.empty() ? "queue" : "queue/" + Replayer(path).header().symbol;
        auto f = std::make_unique<Feed>(path, stream_rng(options_.seed, stream));
        const auto& h = f->replayer.header();
        if (!by_symbol_.emplace(h.symbol, feeds_.size()).second) {
            throw std::invalid_argument("simulator: two files of " + h.symbol);
        }
        SymbolSpec spec;
        spec.symbol = h.symbol;
        spec.tick_size = h.tick_size;
        spec.lot_size = h.lot_size;
        spec.min_qty = h.lot_size;
        spec.min_notional = h.min_notional;
        engine_->set_symbol_spec(spec);
        SynthesizerOptions so{h.symbol, h.tick_size, h.lot_size};
        so.impact_half_life_ns = options_.impact_half_life_ns;
        f->synth = std::make_unique<Synthesizer>(*engine_, *model_, f->queue_rng, so);
        f->permanent = std::make_unique<PermanentImpact>(options_.permanent_impact, h.tick_size);
        symbols.push_back(SymbolInfo{h.symbol, h.tick_size, h.lot_size});
        feeds_.push_back(std::move(f));
    }
    gateway_ = std::make_unique<SimGateway>(*engine_, queue_, std::move(symbols), options_.seed);
    gateway_->set_latency_regime(options_.latency_regime);
    if (options_.impact_half_life_ns || options_.permanent_impact.enabled()) {
        gateway_->set_taker_trades([this](const std::vector<Trade>& trades) { strategy_trades(trades); });
    }
}

void Simulator::strategy_trades(const std::vector<Trade>& trades) {
    if (trades.empty()) {
        return;
    }
    // One request's trades are all on its symbol
    Feed& f = *feeds_[feeds_.size() == 1 ? 0 : by_symbol_.at(trades.front().symbol)];
    f.synth->strategy_trades(trades);
    if (!f.permanent->enabled()) {
        return;
    }
    // Only liquidity taken from the market moves it, not trades between strategies
    for (const Trade& t : trades) {
        if (Synthesizer::is_synthetic_id(t.maker_order_id)) {
            f.permanent->take(t.aggressor_side, t.quantity, t.price);
        }
    }
    f.synth->set_shift(f.permanent->shift_ticks());
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

    uint64_t data_time = 0;  // time of the last data batch, of any symbol; data never moves the clock backwards
    auto batch_time = [&](const Batch& x) { return std::max(data_time, ts_of(x)); };
    auto read = [&](size_t i) {
        Feed& f = *feeds_[i];
        f.have = f.replayer.next(f.next);
        f.next.symbol = static_cast<uint32_t>(i);
    };
    // The feed whose next batch comes first (the earlier file at equal times); feeds_.size() when all ended
    auto earliest = [&]() {
        size_t best = feeds_.size();
        for (size_t i = 0; i < feeds_.size(); ++i) {
            if (feeds_[i]->have && (best == feeds_.size() || ts_of(feeds_[i]->next) < ts_of(feeds_[best]->next))) {
                best = i;
            }
        }
        return best;
    };
    for (size_t i = 0; i < feeds_.size(); ++i) read(i);

    // Fast-forward to the window's start
    size_t i = earliest();
    const uint64_t start = options_.from_ns ? options_.from_ns : (i < feeds_.size() ? batch_time(feeds_[i]->next) : 0);
    while (i < feeds_.size() && batch_time(feeds_[i]->next) < start) {
        Feed& f = *feeds_[i];
        data_time = batch_time(f.next);
        clock_->set(data_time);
        f.synth->apply(f.next);
        gateway_->observe(f.next);
        ++stats_.fast_forward_batches;
        read(i);
        i = earliest();
    }
    clock_->set(std::max(clock_->now_ns(), start));

    uint64_t end = options_.to_ns ? options_.to_ns : kOpen;
    stats_.start_ns = start;
    stats_.trading_ns = start + options_.warmup_ns;
    gateway_->set_trading_from(stats_.trading_ns);
    next_sample_ = start;

    // Each feed has its next batch in the queue, one at a time
    size_t live = 0;
    for (size_t k = 0; k < feeds_.size(); ++k) {
        if (feeds_[k]->have) {
            queue_.push(batch_time(feeds_[k]->next), EventClass::MARKET_DATA,
                        SimEvent{static_cast<uint32_t>(k), feeds_[k]->next});
            ++live;
        }
    }
    if (live == 0 && end == kOpen) {
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
        Batch batch = std::get<Batch>(e.payload.body);
        Feed& f = *feeds_[batch.symbol];
        f.synth->apply(batch);
        batch.shift = f.synth->shift();  // the strategies see the market as it now stands at the exchange
        gateway_->publish(batch);
        ++stats_.data_batches;
        read(batch.symbol);
        if (f.have) {
            queue_.push(batch_time(f.next), EventClass::MARKET_DATA, SimEvent{batch.symbol, f.next});
        } else if (--live == 0 && end == kOpen) {
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
