#include "risk_guard.hpp"

#include "int128.hpp"
#include "types.hpp"

#include <algorithm>
#include <optional>

namespace sim {

namespace {

// The strategy's mark: the observed mid, else the last trade
std::optional<Price> mark(const ObservedBook& book) {
    if (book.valid()) {
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        if (bid && ask) return (bid->price + ask->price) / 2;
    }
    if (const auto t = book.last_trade()) return t->price;
    return std::nullopt;
}

} // namespace

RiskVerdict RiskGuard::check_order(Side side, OrderType type, Price price, Quantity quantity, const Account& account,
                                   const std::map<OrderId, OpenOrder>& orders, const ObservedBook& book,
                                   const OrderId* replacing) const {
    // What else could fill on this side
    Quantity same_side = 0;
    for (const auto& [id, o] : orders) {
        if (o.side == side && !(replacing && id == *replacing)) same_side += o.leaves;
    }
    // The position if all of it filled (positive: in the order's direction)
    const int64_t signed_pos = side == Side::BUY ? account.position : -account.position;
    const int64_t worst = signed_pos + same_side + quantity;

    if (killed()) {
        // Only reducing: against the position, and not past flat
        if (signed_pos >= 0 || worst > 0) {
            return {ExecReason::RISK_KILLED, "Risk: drawdown kill switch is on; only orders that reduce the position"};
        }
        return {};
    }
    if (limits_.max_open_orders && !replacing && orders.size() >= limits_.max_open_orders) {
        return {ExecReason::RISK_OPEN_ORDERS,
                "Risk: maximum of " + std::to_string(limits_.max_open_orders) + " open orders"};
    }
    if (limits_.max_order_notional) {
        std::optional<Price> px;
        if (type != OrderType::MARKET) {
            px = price;
        } else if (book.valid()) {
            if (auto l = side == Side::BUY ? book.best_ask() : book.best_bid()) px = l->price;
        }
        if (px && *px > 0 && quantity > 0 &&
            int128::less(int128::mul(static_cast<uint64_t>(limits_.max_order_notional), kFixedScale),
                         int128::mul(static_cast<uint64_t>(*px), static_cast<uint64_t>(quantity)))) {
            return {ExecReason::RISK_NOTIONAL,
                    "Risk: order notional above " + format_fixed(limits_.max_order_notional)};
        }
    }
    if (limits_.max_position && worst > limits_.max_position) {
        return {ExecReason::RISK_POSITION, "Risk: position could reach " + format_fixed(worst) + ", limit " +
                                               format_fixed(limits_.max_position)};
    }
    return {};
}

bool RiskGuard::update(const Account& account, const ObservedBook& book, uint64_t now) {
    if (!limits_.max_drawdown || killed()) return false;
    const auto m = mark(book);
    if (!m) return false;
    const int64_t pnl = account.pnl(*m);
    peak_ = std::max(peak_, pnl);
    max_drawdown_seen_ = std::max(max_drawdown_seen_, peak_ - pnl);
    if (peak_ - pnl >= limits_.max_drawdown) {
        killed_at_ = std::max<uint64_t>(now, 1);
        return true;
    }
    return false;
}

} // namespace sim
