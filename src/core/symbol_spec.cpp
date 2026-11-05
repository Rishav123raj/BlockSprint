#include "symbol_spec.hpp"
#include "int128.hpp"

namespace {

using int128::U128;
using int128::less;

std::string text(const Order& order, const std::string& what) {
    return "Invalid order '" + order.order_id + "' for " + order.symbol + ": " + what;
}

bool priced(OrderType type) {
    return type != OrderType::MARKET;
}

} // namespace

bool notional_at_least(Price price, Quantity quantity, int64_t min_notional) {
    if (min_notional <= 0) {
        return true;
    }
    if (price <= 0 || quantity <= 0) {
        return false;
    }
    // price and quantity are both in 1e-8 units, so price * quantity is in 1e-16 units
    U128 notional = int128::mul(static_cast<uint64_t>(price), static_cast<uint64_t>(quantity));
    U128 required = int128::mul(static_cast<uint64_t>(min_notional), static_cast<uint64_t>(kFixedScale));
    return !less(notional, required);
}

std::optional<ValidationError> validate_order(const Order& order, const SymbolSpec* spec) {
    if (order.quantity <= 0) {
        return ValidationError{ExecReason::INVALID_QUANTITY, text(order, "quantity must be greater than zero")};
    }
    if (priced(order.order_type) && order.price <= 0) {
        return ValidationError{ExecReason::INVALID_PRICE,
                               text(order, orderTypeToString(order.order_type) + " orders need a price greater than zero")};
    }
    if (!spec || order.market_data_liquidity) {
        return std::nullopt;
    }
    if (priced(order.order_type) && spec->tick_size > 0 && order.price % spec->tick_size != 0) {
        return ValidationError{ExecReason::PRICE_TICK, text(order, "price " + format_fixed(order.price) +
                                                                       " is not a multiple of the tick size " +
                                                                       format_fixed(spec->tick_size))};
    }
    if (spec->lot_size > 0 && order.quantity % spec->lot_size != 0) {
        return ValidationError{ExecReason::QUANTITY_LOT, text(order, "quantity " + format_fixed(order.quantity) +
                                                                         " is not a multiple of the lot size " +
                                                                         format_fixed(spec->lot_size))};
    }
    if (order.quantity < spec->min_qty || (spec->max_qty > 0 && order.quantity > spec->max_qty)) {
        return ValidationError{ExecReason::QUANTITY_RANGE,
                               text(order, "quantity " + format_fixed(order.quantity) + " is outside [" +
                                               format_fixed(spec->min_qty) + ", " +
                                               (spec->max_qty > 0 ? format_fixed(spec->max_qty) : "no limit") + "]")};
    }
    if (priced(order.order_type) && !notional_at_least(order.price, order.quantity, spec->min_notional)) {
        return ValidationError{ExecReason::MIN_NOTIONAL,
                               text(order, "notional (price x quantity) is below the minimum of " +
                                               format_fixed(spec->min_notional))};
    }
    return std::nullopt;
}
