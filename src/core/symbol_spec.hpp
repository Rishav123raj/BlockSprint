#pragma once
#include "events.hpp"
#include "models.hpp"
#include <optional>
#include <string>

// Exchange trading rules for one symbol (P1.12), mirroring Binance's filters:
// PRICE_FILTER.tickSize, LOT_SIZE.stepSize / minQty / maxQty and NOTIONAL.minNotional.
// All values are fixed-point (1e-8 units); 0 means "no rule".
struct SymbolSpec {
    std::string symbol;
    Price tick_size = 0;      // price must be a multiple of this
    Quantity lot_size = 0;    // quantity must be a multiple of this
    Quantity min_qty = 0;
    Quantity max_qty = 0;
    int64_t min_notional = 0; // price x quantity, in quote units (fixed-point)
};

struct ValidationError {
    ExecReason reason;
    std::string text;
};

// Checks an incoming order. Without a spec (or for market-data liquidity) only the basic rules apply:
// quantity > 0, and price > 0 for LIMIT, IOC and FOK. MARKET orders are not checked against tick size
// or min notional (their execution price is unknown when they arrive).
std::optional<ValidationError> validate_order(const Order& order, const SymbolSpec* spec);

// True if price x quantity >= min_notional, computed exactly (128-bit intermediate)
bool notional_at_least(Price price, Quantity quantity, int64_t min_notional);
