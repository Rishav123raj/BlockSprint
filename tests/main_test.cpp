#include "test_harness.hpp"
#include "matching_engine.hpp"
#include "logger.hpp"
#include <memory>

// Reset the singleton MatchingEngineManager between tests
void reset_engine() {
    // We can simulate reset by submitting orders to cancel everything or just creating a new book.
    // Since books_ is private in MatchingEngineManager, we can implement a reset method, or just use unique symbols per test!
    // Using unique symbols per test (e.g. "SYM-PTP", "SYM-TTP", "SYM-IOC", "SYM-FOK") is a brilliant, zero-modification way to isolate tests!
}

TEST(price_time_priority) {
    std::string symbol = "SYM-PTP";
    
    // 1. Submit resting buy limit A at 100.0 qty 10.0
    auto a = std::make_shared<Order>();
    a->order_id = "A";
    a->symbol = symbol;
    a->order_type = OrderType::LIMIT;
    a->side = Side::BUY;
    a->price = 100.0;
    a->quantity = 10.0;
    
    auto trades_a = MatchingEngineManager::getInstance().submit_order(a);
    ASSERT_EQ(trades_a.size(), 0U);

    // 2. Submit resting buy limit B at 100.0 qty 5.0
    auto b = std::make_shared<Order>();
    b->order_id = "B";
    b->symbol = symbol;
    b->order_type = OrderType::LIMIT;
    b->side = Side::BUY;
    b->price = 100.0;
    b->quantity = 5.0;

    auto trades_b = MatchingEngineManager::getInstance().submit_order(b);
    ASSERT_EQ(trades_b.size(), 0U);

    // Assert BBO has correct total bid quantity
    nlohmann::json bbo = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo["bid_price"].get<std::string>(), "100.000000");
    ASSERT_EQ(bbo["bid_qty"].get<std::string>(), "15.000000");

    // 3. Submit marketable sell limit C at 100.0 qty 12.0
    auto c = std::make_shared<Order>();
    c->order_id = "C";
    c->symbol = symbol;
    c->order_type = OrderType::LIMIT;
    c->side = Side::SELL;
    c->price = 100.0;
    c->quantity = 12.0;

    auto trades_c = MatchingEngineManager::getInstance().submit_order(c);
    // Should result in 2 trades (10.0 with A, 2.0 with B due to price-time priority)
    ASSERT_EQ(trades_c.size(), 2U);

    ASSERT_EQ(trades_c[0].maker_order_id, "A");
    ASSERT_EQ(trades_c[0].taker_order_id, "C");
    ASSERT_DOUBLE_EQ(trades_c[0].quantity, 10.0);
    ASSERT_DOUBLE_EQ(trades_c[0].price, 100.0);

    ASSERT_EQ(trades_c[1].maker_order_id, "B");
    ASSERT_EQ(trades_c[1].taker_order_id, "C");
    ASSERT_DOUBLE_EQ(trades_c[1].quantity, 2.0);
    ASSERT_DOUBLE_EQ(trades_c[1].price, 100.0);

    // B should have 3.0 remaining
    nlohmann::json bbo_after = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo_after["bid_price"].get<std::string>(), "100.000000");
    ASSERT_EQ(bbo_after["bid_qty"].get<std::string>(), "3.000000");
}

TEST(internal_trade_through_protection) {
    std::string symbol = "SYM-TTP";

    // 1. Submit sell limit A at 101.0 qty 10.0 (Best Ask)
    auto a = std::make_shared<Order>();
    a->order_id = "A";
    a->symbol = symbol;
    a->order_type = OrderType::LIMIT;
    a->side = Side::SELL;
    a->price = 101.0;
    a->quantity = 10.0;
    MatchingEngineManager::getInstance().submit_order(a);

    // 2. Submit sell limit B at 102.0 qty 10.0 (Worst Ask)
    auto b = std::make_shared<Order>();
    b->order_id = "B";
    b->symbol = symbol;
    b->order_type = OrderType::LIMIT;
    b->side = Side::SELL;
    b->price = 102.0;
    b->quantity = 10.0;
    MatchingEngineManager::getInstance().submit_order(b);

    // 3. Submit marketable buy limit C at 103.0 qty 15.0
    // C must fill completely at the best available price (101.0) first, and then move to 102.0.
    // Price-time and internal protection check.
    auto c = std::make_shared<Order>();
    c->order_id = "C";
    c->symbol = symbol;
    c->order_type = OrderType::LIMIT;
    c->side = Side::BUY;
    c->price = 103.0;
    c->quantity = 15.0;

    auto trades = MatchingEngineManager::getInstance().submit_order(c);
    ASSERT_EQ(trades.size(), 2U);

    // First trade should sweep 101.0
    ASSERT_EQ(trades[0].maker_order_id, "A");
    ASSERT_DOUBLE_EQ(trades[0].price, 101.0);
    ASSERT_DOUBLE_EQ(trades[0].quantity, 10.0);

    // Second trade should sweep 102.0
    ASSERT_EQ(trades[1].maker_order_id, "B");
    ASSERT_DOUBLE_EQ(trades[1].price, 102.0);
    ASSERT_DOUBLE_EQ(trades[1].quantity, 5.0);

    // Best ask should now be 102.0 with remaining qty 5.0
    nlohmann::json bbo = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo["ask_price"].get<std::string>(), "102.000000");
    ASSERT_EQ(bbo["ask_qty"].get<std::string>(), "5.000000");
}

TEST(ioc_order_handling) {
    std::string symbol = "SYM-IOC";

    // 1. Place ask at 100.0, qty 10.0
    auto a = std::make_shared<Order>();
    a->order_id = "A";
    a->symbol = symbol;
    a->order_type = OrderType::LIMIT;
    a->side = Side::SELL;
    a->price = 100.0;
    a->quantity = 10.0;
    MatchingEngineManager::getInstance().submit_order(a);

    // 2. Submit buy IOC at 100.0, qty 15.0
    auto ioc = std::make_shared<Order>();
    ioc->order_id = "IOC_BUY";
    ioc->symbol = symbol;
    ioc->order_type = OrderType::IOC;
    ioc->side = Side::BUY;
    ioc->price = 100.0;
    ioc->quantity = 15.0;

    auto trades = MatchingEngineManager::getInstance().submit_order(ioc);
    // Should match 10.0 and cancel remaining 5.0 without resting on the book.
    ASSERT_EQ(trades.size(), 1U);
    ASSERT_DOUBLE_EQ(trades[0].quantity, 10.0);

    // The order book should now be completely empty on bids and asks
    nlohmann::json bbo = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo["bid_price"].get<std::string>(), "0.0");
    ASSERT_EQ(bbo["ask_price"].get<std::string>(), "0.0");
}

TEST(fok_order_handling) {
    std::string symbol = "SYM-FOK";

    // 1. Place ask at 100.0, qty 10.0
    auto a = std::make_shared<Order>();
    a->order_id = "A";
    a->symbol = symbol;
    a->order_type = OrderType::LIMIT;
    a->side = Side::SELL;
    a->price = 100.0;
    a->quantity = 10.0;
    MatchingEngineManager::getInstance().submit_order(a);

    // 2. Submit FOK at 100.0, qty 15.0. Needs 15.0 but book has 10.0. FOK must fail and kill itself.
    auto fok_fail = std::make_shared<Order>();
    fok_fail->order_id = "FOK_FAIL";
    fok_fail->symbol = symbol;
    fok_fail->order_type = OrderType::FOK;
    fok_fail->side = Side::BUY;
    fok_fail->price = 100.0;
    fok_fail->quantity = 15.0;

    auto trades_fail = MatchingEngineManager::getInstance().submit_order(fok_fail);
    ASSERT_EQ(trades_fail.size(), 0U); // No trades executed

    // Ask A should remain untouched
    nlohmann::json bbo1 = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo1["ask_price"].get<std::string>(), "100.000000");
    ASSERT_EQ(bbo1["ask_qty"].get<std::string>(), "10.000000");

    // 3. Submit FOK at 100.0, qty 10.0. Matches book exactly.
    auto fok_success = std::make_shared<Order>();
    fok_success->order_id = "FOK_SUCCESS";
    fok_success->symbol = symbol;
    fok_success->order_type = OrderType::FOK;
    fok_success->side = Side::BUY;
    fok_success->price = 100.0;
    fok_success->quantity = 10.0;

    auto trades_success = MatchingEngineManager::getInstance().submit_order(fok_success);
    ASSERT_EQ(trades_success.size(), 1U);
    ASSERT_DOUBLE_EQ(trades_success[0].quantity, 10.0);

    // Book is empty now
    nlohmann::json bbo2 = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo2["ask_price"].get<std::string>(), "0.0");
}

TEST(order_cancellation) {
    std::string symbol = "SYM-CANCEL";

    // 1. Submit resting buy order
    auto a = std::make_shared<Order>();
    a->order_id = "A";
    a->symbol = symbol;
    a->order_type = OrderType::LIMIT;
    a->side = Side::BUY;
    a->price = 100.0;
    a->quantity = 10.0;
    MatchingEngineManager::getInstance().submit_order(a);

    nlohmann::json bbo = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo["bid_price"].get<std::string>(), "100.000000");

    // 2. Cancel order
    bool success = MatchingEngineManager::getInstance().cancel_order(symbol, "A");
    ASSERT_TRUE(success);

    // Book should be empty
    nlohmann::json bbo_after = MatchingEngineManager::getInstance().get_bbo(symbol);
    ASSERT_EQ(bbo_after["bid_price"].get<std::string>(), "0.0");

    // 3. Cancel again, should fail
    bool success_again = MatchingEngineManager::getInstance().cancel_order(symbol, "A");
    ASSERT_FALSE(success_again);
}

TEST(order_modification) {
    std::string symbol = "SYM-MODIFY";

    // 1. Submit buy limit A (100.0, qty 10.0)
    auto a = std::make_shared<Order>();
    a->order_id = "A";
    a->symbol = symbol;
    a->order_type = OrderType::LIMIT;
    a->side = Side::BUY;
    a->price = 100.0;
    a->quantity = 10.0;
    MatchingEngineManager::getInstance().submit_order(a);

    // 2. Submit buy limit B (100.0, qty 5.0)
    auto b = std::make_shared<Order>();
    b->order_id = "B";
    b->symbol = symbol;
    b->order_type = OrderType::LIMIT;
    b->side = Side::BUY;
    b->price = 100.0;
    b->quantity = 5.0;
    MatchingEngineManager::getInstance().submit_order(b);

    // 3. Modify A: Decrease qty to 4.0. Should maintain priority.
    std::vector<Trade> trades_mod;
    bool mod_success = MatchingEngineManager::getInstance().modify_order(symbol, "A", 4.0, 100.0, trades_mod);
    ASSERT_TRUE(mod_success);
    ASSERT_EQ(trades_mod.size(), 0U);

    // 4. Submit Sell limit C (100.0, qty 6.0). 
    // It should match 4.0 against A (since A maintained priority) and 2.0 against B.
    auto c = std::make_shared<Order>();
    c->order_id = "C";
    c->symbol = symbol;
    c->order_type = OrderType::LIMIT;
    c->side = Side::SELL;
    c->price = 100.0;
    c->quantity = 6.0;
    auto trades = MatchingEngineManager::getInstance().submit_order(c);

    ASSERT_EQ(trades.size(), 2U);
    ASSERT_EQ(trades[0].maker_order_id, "A");
    ASSERT_DOUBLE_EQ(trades[0].quantity, 4.0);
    ASSERT_EQ(trades[1].maker_order_id, "B");
    ASSERT_DOUBLE_EQ(trades[1].quantity, 2.0);
}

int main() {
    LOG_INFO("=== Running Crypto Trading Engine C++ Unit Tests ===");

    for (const auto& test : get_tests()) {
        LOG_INFO("Running test: " + test.name);
        try {
            test.func();
            g_test_passes++;
            LOG_INFO("PASS: " + test.name);
        } catch (const std::exception& e) {
            g_test_failures++;
            LOG_ERROR("FAIL: " + test.name + " (" + e.what() + ")");
        }
    }

    LOG_INFO("=== Unit Test Results ===");
    LOG_INFO("Passes: " + std::to_string(g_test_passes));
    LOG_INFO("Failures: " + std::to_string(g_test_failures));

    return g_test_failures > 0 ? 1 : 0;
}
