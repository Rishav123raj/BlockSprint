#include "strategies/passive_quoter.hpp"

#include "strategies/params.hpp"

#include <vector>

namespace sim {

PassiveQuoter::Config PassiveQuoter::config_from(StrategyParams& p) {
    Config c;
    c.size = p.decimal("size", "0.001");
    c.refresh_ns = p.millis("refresh_ms", 500);
    c.max_position = p.decimal("max_position", "0.01");
    c.post_only = p.flag("post_only", true);
    if (c.refresh_ns == 0) p.bad("refresh_ms", "must be > 0");
    if (c.max_position < c.size) p.bad("max_position", "must be at least size");
    return c;
}

void PassiveQuoter::on_start(Context& ctx) {
    requote(ctx);
    ctx.set_timer(c_.refresh_ns);
}

void PassiveQuoter::on_timer(Context& ctx, uint64_t) {
    requote(ctx);
    ctx.set_timer(c_.refresh_ns);
}

void PassiveQuoter::on_gap(Context& ctx) { requote(ctx); }

void PassiveQuoter::requote(Context& ctx) {
    if (!ctx.can_trade()) return;
    ++refreshes_;
    const ObservedBook& book = ctx.book();
    const int64_t position = ctx.account().position;
    std::optional<Price> bid, ask;
    if (book.valid()) {
        if (auto b = book.best_bid(); b && position + c_.size <= c_.max_position) bid = b->price;
        if (auto a = book.best_ask(); a && position - c_.size >= -c_.max_position) ask = a->price;
    }
    quote_side(ctx, Side::BUY, bid ? &*bid : nullptr);
    quote_side(ctx, Side::SELL, ask ? &*ask : nullptr);
}

// target: the price to quote at, or null for no quote on this side
void PassiveQuoter::quote_side(Context& ctx, Side side, const Price* target) {
    std::vector<const OpenOrder*> mine;
    for (const auto& [id, o] : ctx.open_orders()) {
        if (o.side != side) continue;
        if (o.status == OrderStatus::PENDING_NEW || o.status == OrderStatus::PENDING_CANCEL ||
            o.status == OrderStatus::PENDING_REPLACE) {
            return;  // wait for the exchange's answer
        }
        mine.push_back(&o);
    }
    // Copied first: a request changes open_orders()
    const bool have = !mine.empty();
    const OrderId keep = have ? mine[0]->id : OrderId();
    const Price keep_price = have ? mine[0]->price : 0;
    std::vector<OrderId> extra;
    for (size_t i = (target ? 1 : 0); i < mine.size(); ++i) extra.push_back(mine[i]->id);
    for (const auto& id : extra) ctx.cancel(id);
    if (!target) return;
    if (!have) {
        ctx.send(side, c_.post_only ? OrderType::LIMIT_MAKER : OrderType::LIMIT, *target, c_.size);
    } else if (keep_price != *target) {
        ctx.replace(keep, *target, c_.size);
    }
}

} // namespace sim
