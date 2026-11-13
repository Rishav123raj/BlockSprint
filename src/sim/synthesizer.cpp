#include "synthesizer.hpp"

#include <algorithm>
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
    : engine_(engine), model_(model), rng_(rng), options_(std::move(options)),
      impact_(options_.impact_half_life_ns, options_.lot_size > 0 ? options_.lot_size : 1) {
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

void Synthesizer::strategy_trades(const std::vector<Trade>& trades) {
    if (!impact_.enabled() || !synced_) {
        return;
    }
    const uint64_t now = engine_.clock().now_ns();
    for (const Trade& t : trades) {
        if (!is_synthetic_id(t.maker_order_id) || t.price % options_.tick_size != 0) {
            continue;  // another strategy's order, or off the data's grid
        }
        // The aggressor's opposite side rested: a buyer takes asks
        const uint8_t side = t.aggressor_side == Side::BUY ? bse::ASK : bse::BID;
        impact_.consume(side, t.price / options_.tick_size, t.quantity, now);
        stats_.impact_taken_qty += t.quantity;
    }
}

int64_t Synthesizer::target_qty(const Change& c) {
    const int64_t data = data_.qty(c.side, c.px) * options_.lot_size;
    if (!impact_.enabled() || impact_.levels() == 0) {
        return data;
    }
    if (data == 0) {
        impact_.reset(c.side, c.px);
        return 0;
    }
    const int64_t held = std::min(data, impact_.outstanding(c.side, c.px, engine_.clock().now_ns()));
    stats_.impact_held_qty += held;
    return data - held;
}

void Synthesizer::apply_depth(const Batch& batch) {
    ++stats_.depth_batches;
    if (!synced_) {
        ++stats_.depth_unsynced;
        return;
    }
    changes_.clear();
    for (const bse::Record* r = batch.begin; r < batch.end; ++r) {
        data_.set(r->side, r->px + shift_, r->qty);
        changes_.push_back(Change{r->side, r->px + shift_});
    }
    reconcile(changes_unique());
}

void Synthesizer::set_shift(int64_t ticks) {
    if (ticks == shift_) {
        return;
    }
    const int64_t d = ticks - shift_;
    shift_ = ticks;
    ++stats_.shifts;
    if (!synced_) {
        return;  // nothing to move; the next snapshot comes in moved
    }
    changes_.clear();
    moved_.clear();
    for (uint8_t side = 0; side < 2; ++side) {
        data_.top(side, kAllLevels, levels_);
        for (const Level& level : levels_) {
            changes_.push_back(Change{side, level.px});
            if (level.px + d > 0) moved_.emplace_back(side, Level{level.px + d, level.qty});
        }
    }
    data_.clear();
    impact_.clear();
    for (const auto& [side, level] : moved_) {
        data_.set(side, level.px, level.qty);
        changes_.push_back(Change{side, level.px});
    }
    reconcile(false);  // old and new prices overlap
}

// One depth event lists each price once; checked rather than assumed (hand-written files may not)
bool Synthesizer::changes_unique() {
    keys_.clear();
    for (const Change& c : changes_) {
        keys_.push_back((static_cast<uint64_t>(c.side) << 63) | static_cast<uint64_t>(c.px));
    }
    std::sort(keys_.begin(), keys_.end());
    return std::adjacent_find(keys_.begin(), keys_.end()) == keys_.end();
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
    impact_.clear();
    for (const bse::Record* r = batch.begin + 1; r + 1 < batch.end; ++r) {
        data_.set(r->side, r->px + shift_, r->qty);
        changes_.push_back(Change{r->side, r->px + shift_});
    }
    synced_ = true;
    reconcile(false);  // a level the book had and the snapshot has is listed twice
}

void Synthesizer::reconcile(bool unique) {
    // Decreases first: until every level that shrinks has shrunk, an addition could cross a stale level.
    // A change at one price never alters the synthetic quantity at another (synthetic orders don't trade
    // with each other; a crossing fill takes strategy orders only), so with unique levels the quantity left
    // after the decreases is what the increases start from, without reading the level again.
    after_decreases_.resize(changes_.size());
    targets_.resize(changes_.size());
    for (size_t i = 0; i < changes_.size(); ++i) {
        const Change& c = changes_[i];
        const Price price = c.px * options_.tick_size;
        const int64_t target = targets_[i] = target_qty(c);
        int64_t synthetic = scan(book_side(c.side), price);
        if (synthetic > target) {
            const int64_t removed = remove_scanned(price, synthetic, synthetic - target, false);
            stats_.cancelled_qty += removed;
            synthetic -= removed;
        }
        after_decreases_[i] = synthetic;
    }
    for (size_t i = 0; i < changes_.size(); ++i) {
        const Change& c = changes_[i];
        const Side side = book_side(c.side);
        const Price price = c.px * options_.tick_size;
        const int64_t target = targets_[i];
        const int64_t synthetic = unique ? after_decreases_[i] : scan(side, price);
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
    const Price price = (r.px + shift_) * options_.tick_size;
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
    int64_t at_price = 0, filled = 0;
    for (const Trade& t : trades_) {
        filled += t.quantity;
        if (!is_synthetic_id(t.maker_order_id)) {
            stats_.tape_strategy_qty += t.quantity;
            continue;
        }
        stats_.tape_synthetic_qty += t.quantity;
        if (t.price == price) {
            at_price += t.quantity;
        } else {
            stats_.tape_through_qty += t.quantity;
        }
    }
    stats_.tape_short_qty += qty - filled;
    stats_.trades_exact += at_price == qty;

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
    impact_.clear();
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
    return remove_scanned(price, scan(side, price), amount, from_front);
}

int64_t Synthesizer::remove_scanned(Price price, int64_t synthetic, int64_t amount, bool from_front) {
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
