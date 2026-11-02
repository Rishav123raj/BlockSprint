#pragma once
#include "matching_engine.hpp"
#include <memory>
#include <string>

// Fixed-point value from a human-readable number: fx(100.5) == 10050000000. Tests use values that are
// exact in binary floating point, so the conversion is exact.
inline int64_t fx(double value) { return from_double(value); }

// Builds an order without submitting it. Price and quantity are given as plain numbers.
inline std::shared_ptr<Order> make_order(const std::string& id, const std::string& symbol, OrderType type, Side side,
                                         double price, double quantity) {
    auto o = std::make_shared<Order>();
    o->order_id = id;
    o->symbol = symbol;
    o->order_type = type;
    o->side = side;
    o->price = fx(price);
    o->quantity = fx(quantity);
    return o;
}

// Fixture: every test case gets its own engine (wall clock, empty books, no callbacks, trade IDs from 1).
struct EngineFixture {
    MatchingEngine engine_;

    MatchingEngine& engine() { return engine_; }

    std::vector<Trade> submit(const std::string& id, const std::string& symbol, OrderType type, Side side,
                              double price, double quantity) {
        return engine_.submit_order(make_order(id, symbol, type, side, price, quantity));
    }
};
