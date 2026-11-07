#include "synthesizer.hpp"

#include <limits>
#include <memory>
#include <stdexcept>

namespace sim {

namespace {

// Book side of a data level (0 bid, 1 ask)
Side book_side(uint8_t side) { return side == bse::BID ? Side::BUY : Side::SELL; }

constexpr size_t kAllLevels = std::numeric_limits<size_t>::max();

} // namespace

Synthesizer::Synthesizer(MatchingEngine& engine, const QueueModel& model, Rng& rng, SynthesizerOptions options)
    : engine_(engine), model_(model), rng_(rng), options_(std::move(options)) {
    if (options_.symbol.empty() || options_.tick_size <= 0 || options_.lot_size <= 0) {
        throw std::invalid_argument("Synthesizer needs a symbol and positive tick and lot sizes");
    }
    if (options_.market_owner.empty() || options_.market_owner == options_.tape_owner) {
        throw std::invalid_argument("Synthesizer: the market and tape owners must differ and not be empty");
    }
}

std::string Synthesizer::next_id() { return "~" + std::to_string(++id_seq_); }

void Synthesizer::apply(const Batch& batch) {
    switch (batch.kind) {
        case BatchKind::DEPTH:
            apply_depth(batch);
            break;
        case BatchKind::SNAPSHOT:
            apply_snapshot(batch);
            break;
        case BatchKind::TRADE:
            apply_trade(*batch.begin);
            break;
        case BatchKind::GAP:
            apply_gap();
            break;
    }
}

void Synthesizer::apply_depth(const Batch& batch) {
    ++stats_.depth_batches;
    if (!synced_) {
        ++stats_.depth_unsynced;
        return;
    }
    changes_.clear();
    for (const bse::Record* r = batch.begin; r < batch.end; ++r) {
        data_.set(r->side, r->px, r->qty);
        changes_.push_back(Change{r->side, r->px});
    }
    reconcile();
}

void Synthesizer::apply_snapshot(const Batch& batch) {
    ++stats_.snapshots;
    changes_.clear();
    // Every level the book had or the snapshot has is a change; the ones it doesn't list go to zero
    for (uint8_t side = 0; side < 2; ++side) {
        data_.top(side, kAllLevels, levels_);
        for (const Level& level : levels_) {
            changes_.push_back(Change{side, level.px});
        }
    }
    data_.clear();
    for (const bse::Record* r = batch.begin + 1; r + 1 < batch.end; ++r) {
        data_.set(r->side, r->px, r->qty);
        changes_.push_back(Change{r->side, r->px});
    }
    synced_ = true;
    reconcile();
}

void Synthesizer::reconcile() {
    // Decreases first: until every level that shrinks has shrunk, an addition could cross a stale level
    for (const Change& c : changes_) {
        const Side side = book_side(c.side);
        const Price price = c.px * options_.tick_size;
        const int64_t target = data_.qty(c.side, c.px) * options_.lot_size;
        const int64_t synthetic = scan(side, price);
        if (synthetic > target) {
            stats_.cancelled_qty += remove(side, price, synthetic - target, false);
        }
    }
    for (const Change& c : changes_) {
        const Side side = book_side(c.side);
        const Price price = c.px * options_.tick_size;
        const int64_t target = data_.qty(c.side, c.px) * options_.lot_size;
        const int64_t synthetic = scan(side, price);
        if (synthetic < target) {
            add(side, price, target - synthetic);
        }
    }
}

void Synthesizer::apply_trade(const bse::Record& r) {
    ++stats_.trades;
    if (!synced_) {
        ++stats_.trades_unsynced;
        return;
    }
    // A trade record's side is the aggressor's: a buyer consumes asks
    const Side aggressor = r.side == bse::BID ? Side::BUY : Side::SELL;
    const Side resting = aggressor == Side::BUY ? Side::SELL : Side::BUY;
    const Price price = r.px * options_.tick_size;
    const int64_t qty = r.qty * options_.lot_size;
    if (qty <= 0 || price <= 0) {
        return;
    }
    stats_.trade_qty += qty;

    const int64_t before = scan(resting, price);
    // One order object serves every tape IOC: an IOC never rests, so the engine keeps no reference to it
    if (!tape_) {
        tape_ = std::make_shared<Order>();
        tape_->symbol = options_.symbol;
        tape_->order_type = OrderType::IOC;
        tape_->owner_id = options_.tape_owner;
        tape_->stp_policy = StpPolicy::NONE;
        tape_->market_data_liquidity = true;
    }
    tape_->order_id = next_id();
    tape_->side = aggressor;
    tape_->price = price;
    tape_->quantity = qty;
    tape_->timestamp = engine_.clock().now_ns();
    trades_ = engine_.submit_order(tape_);
    for (const Trade& t : trades_) {
        (is_synthetic_id(t.maker_order_id) ? stats_.tape_synthetic_qty : stats_.tape_strategy_qty) += t.quantity;
    }

    // The real trade took qty of real liquidity at this price. What the synthetic queue here didn't give
    // (strategy orders, or stale better levels, took it) comes off its front now, so the depth event that
    // reflects the trade finds nothing left to remove.
    const int64_t after = scan(resting, price);
    const int64_t missing = qty - (before - after);
    if (missing > 0 && after > 0) {
        stats_.trade_front_qty += remove(resting, price, missing, true);
    }
}

void Synthesizer::apply_gap() {
    ++stats_.gaps;
    for (uint8_t side = 0; side < 2; ++side) {
        data_.top(side, kAllLevels, levels_);
        for (const Level& level : levels_) {
            remove(book_side(side), level.px * options_.tick_size, std::numeric_limits<int64_t>::max(), true);
        }
    }
    data_.clear();
    synced_ = false;
}

int64_t Synthesizer::scan(Side side, Price price) {
    slots_.clear();
    queue_.clear();
    int64_t synthetic = 0;
    const PriceLevel* level = engine_.find_level(options_.symbol, side, price);
    for (const Order* o = level ? level->head : nullptr; o; o = o->queue_next) {
        slots_.push_back(QueueSlot{o->remaining_quantity, o->market_data_liquidity});
        queue_.push_back(o);
        if (o->market_data_liquidity) {
            synthetic += o->remaining_quantity;
        }
    }
    return synthetic;
}

int64_t Synthesizer::synthetic_qty(Side side, Price price) { return scan(side, price); }

int64_t Synthesizer::remove(Side side, Price price, int64_t amount, bool from_front) {
    const int64_t synthetic = scan(side, price);
    amount = std::min(amount, synthetic);
    if (amount <= 0) {
        return 0;
    }
    if (from_front) {
        take_.assign(slots_.size(), 0);
        detail::take_in_order(slots_, 0, slots_.size(), false, amount, take_);
    } else {
        model_.choose(slots_, amount, options_.lot_size, rng_, take_);
    }
    int64_t removed = 0;
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (take_[i] <= 0 || !slots_[i].synthetic) {
            continue;
        }
        // Each call changes only its own order, so the other pointers stay valid
        const Order& o = *queue_[i];
        const std::string id = o.order_id;
        const int64_t left = o.remaining_quantity - take_[i];
        if (left <= 0) {
            engine_.cancel_order(options_.symbol, id);
        } else {
            engine_.modify_order(options_.symbol, id, left, price, trades_);  // a decrease keeps its place
        }
        removed += std::min(take_[i], slots_[i].qty);
    }
    return removed;
}

void Synthesizer::add(Side side, Price price, int64_t amount) {
    stats_.added_qty += amount;
    const PriceLevel* level = engine_.find_level(options_.symbol, side, price);
    if (level && level->tail && level->tail->market_data_liquidity) {
        // Merged into the synthetic order at the back: an increase re-queues it at the back, where it is
        // already. The level exists on this side, so nothing can cross it.
        const Order& tail = *level->tail;
        const std::string id = tail.order_id;
        engine_.modify_order(options_.symbol, id, tail.remaining_quantity + amount, price, trades_);
        return;
    }
    auto order = std::make_shared<Order>();
    order->order_id = next_id();
    order->symbol = options_.symbol;
    order->order_type = OrderType::LIMIT;
    order->side = side;
    order->price = price;
    order->quantity = amount;
    order->timestamp = engine_.clock().now_ns();
    order->owner_id = options_.market_owner;
    order->stp_policy = StpPolicy::SKIP;  // never trades with other synthetic orders
    order->market_data_liquidity = true;
    trades_ = engine_.submit_order(order);
    for (const Trade& t : trades_) {  // crossing fills: only strategy orders can be on the other side
        ++stats_.crossing_fills;
        stats_.crossing_qty += t.quantity;
    }
    if (!order->queued && order->remaining_quantity > 0) {
        stats_.would_cross_qty += order->remaining_quantity;
    }
}

std::vector<std::string> Synthesizer::check(bool exact) {
    std::vector<std::string> violations;
    const Depth depth = engine_.depth(options_.symbol, std::numeric_limits<int>::max());
    auto describe = [](Side side, Price price) {
        return std::string(side == Side::BUY ? "bid " : "ask ") + format_fixed(price);
    };
    for (const auto& [side, levels] : {std::pair<Side, const std::vector<DepthLevel>*>{Side::BUY, &depth.bids},
                                       std::pair<Side, const std::vector<DepthLevel>*>{Side::SELL, &depth.asks}}) {
        const uint8_t data_side = side == Side::BUY ? bse::BID : bse::ASK;
        for (const DepthLevel& level : *levels) {
            const int64_t synthetic = scan(side, level.price);
            const bool on_grid = level.price % options_.tick_size == 0;
            const int64_t data = on_grid ? data_.qty(data_side, level.price / options_.tick_size) * options_.lot_size : 0;
            if (synthetic > data) {
                violations.push_back(describe(side, level.price) + ": synthetic " + format_fixed(synthetic) +
                                     " exceeds the data's " + format_fixed(data));
            }
            if (exact && level.quantity != synthetic) {
                violations.push_back(describe(side, level.price) + ": level " + format_fixed(level.quantity) +
                                     " holds non-synthetic orders (synthetic " + format_fixed(synthetic) + ")");
            }
        }
    }
    if (exact) {
        for (uint8_t data_side = 0; data_side < 2; ++data_side) {
            data_.top(data_side, kAllLevels, levels_);
            for (const Level& level : levels_) {
                const Side side = book_side(data_side);
                const Price price = level.px * options_.tick_size;
                const int64_t synthetic = scan(side, price);
                if (synthetic != level.qty * options_.lot_size) {
                    violations.push_back(describe(side, price) + ": synthetic " + format_fixed(synthetic) +
                                         " != data " + format_fixed(level.qty * options_.lot_size));
                }
            }
        }
    }
    return violations;
}

} // namespace sim
