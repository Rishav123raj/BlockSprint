#include "strategies/twap_taker.hpp"

#include "strategies/params.hpp"

#include <algorithm>

namespace sim {

namespace {
constexpr uint64_t kStartPollNs = 100000000;  // 100 ms, while waiting for the warm-up to end
}

TwapTaker::Config TwapTaker::config_from(StrategyParams& p) {
    Config c;
    const std::string side = p.text("side", "buy");
    if (side == "buy") {
        c.side = Side::BUY;
    } else if (side == "sell") {
        c.side = Side::SELL;
    } else {
        p.bad("side", "expected \"buy\" or \"sell\", got \"" + side + "\"");
    }
    c.quantity = p.decimal("quantity", "0.01");
    c.slices = p.integer("slices", 10);
    c.duration_ns = p.millis("duration_ms", 600000);
    if (c.slices == 0 || c.slices > 1000000) p.bad("slices", "expected 1 to 1000000");
    if (c.duration_ns < c.slices) p.bad("duration_ms", "too short for the number of slices");
    return c;
}

void TwapTaker::on_start(Context& ctx) { tick(ctx); }

void TwapTaker::on_timer(Context& ctx, uint64_t) { tick(ctx); }

void TwapTaker::on_exec(Context&, const ExecReport& r) {
    if (r.type == ExecType::FILL) {
        filled_ += r.last_qty;
    } else if (r.type == ExecType::CANCELLED || r.type == ExecType::REJECTED) {
        committed_ -= r.quantity - r.cum_qty;  // came back unfilled
    }
}

// quantity x due / slices, rounded down to the lot (exact without overflow: slices <= 10^6); all of it
// once every slice is due
Quantity TwapTaker::target(uint64_t slices_due, Quantity lot) const {
    if (slices_due >= c_.slices) return c_.quantity;
    const auto n = static_cast<int64_t>(c_.slices);
    const auto k = static_cast<int64_t>(slices_due);
    const int64_t q = (c_.quantity / n) * k + (c_.quantity % n) * k / n;
    return lot > 0 ? q / lot * lot : q;
}

void TwapTaker::tick(Context& ctx) {
    if (filled_ >= c_.quantity) return;  // done: no more timers
    if (!started_) {
        if (!ctx.can_trade()) {
            ctx.set_timer(std::min(interval(), kStartPollNs));
            return;
        }
        started_ = true;
        start_ns_ = ctx.now();
    }
    due_ = std::min(due_ + 1, c_.slices);
    const Quantity want = target(due_, ctx.book().lot_size()) - committed_;
    if (want > 0 && ctx.book().valid()) {
        ctx.send(c_.side, OrderType::MARKET, 0, want);
        committed_ += want;
    }
    ctx.set_timer(interval());
}

} // namespace sim
