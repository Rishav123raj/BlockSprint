#pragma once
// RiskGuard (P6.4): pre-trade limits on a strategy's own requests, checked in its Context before they enter
// the latency pipe, against what the strategy knows (its account, its open orders, its observed book):
//
//   max_position        |position| if the order and every other open order on its side filled
//   max_order_notional  price x quantity of one order (a MARKET order at the observed best opposite price)
//   max_open_orders     open and pending orders, for a new order
//   max_drawdown        kill switch: when PnL at the observed mid falls this far below its running high,
//                       every open order is cancelled and from then on only orders that reduce the
//                       position (without reversing it, counting the open orders on their side) pass
//
// Every limit is off at 0. A refused request never reaches the exchange; the strategy hears of it at once
// (a REJECTED report for a new order, a request reject for an amend or replace), with a risk_* reason.
// Cancels always pass, and so does an amend that doesn't increase the order's quantity.
#include "observed_book.hpp"
#include "strategy.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace sim {

struct RiskLimits {
    Quantity max_position = 0;       // base units (1e-8)
    int64_t max_order_notional = 0;  // quote units (1e-8)
    uint32_t max_open_orders = 0;
    int64_t max_drawdown = 0;        // quote units (1e-8)

    bool any() const { return max_position || max_order_notional || max_open_orders || max_drawdown; }
};

struct RiskVerdict {
    ExecReason reason = ExecReason::NONE;
    std::string text;

    bool ok() const { return reason == ExecReason::NONE; }
};

class RiskGuard {
public:
    explicit RiskGuard(RiskLimits limits) : limits_(limits) {}

    // A new order, or the new version of `replacing` (a replace or an amend): its other open orders count,
    // that one doesn't
    RiskVerdict check_order(Side side, OrderType type, Price price, Quantity quantity, const Account& account,
                            const std::map<OrderId, OpenOrder>& orders, const ObservedBook& book,
                            const OrderId* replacing = nullptr) const;

    // Marks the strategy's PnL at the observed mid (no-op without max_drawdown or a mark); true when this
    // call trips the kill switch
    bool update(const Account& account, const ObservedBook& book, uint64_t now);

    const RiskLimits& limits() const { return limits_; }
    bool killed() const { return killed_at_ != 0; }
    uint64_t killed_at() const { return killed_at_; }
    int64_t max_drawdown_seen() const { return max_drawdown_seen_; }

private:
    RiskLimits limits_;
    int64_t peak_ = 0;  // the PnL's running high (flat at the start: 0)
    int64_t max_drawdown_seen_ = 0;
    uint64_t killed_at_ = 0;
};

} // namespace sim
