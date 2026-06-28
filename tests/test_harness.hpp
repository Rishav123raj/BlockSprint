#pragma once
#include <iostream>
#include <string>
#include <exception>
#include <vector>
#include <cmath>

inline int g_test_failures = 0;
inline int g_test_passes = 0;

struct TestDef {
    std::string name;
    void (*func)();
};

inline std::vector<TestDef>& get_tests() {
    static std::vector<TestDef> tests;
    return tests;
}

inline void register_test(const std::string& name, void (*func)()) {
    get_tests().push_back({name, func});
}

#define TEST(name) \
    void test_func_##name(); \
    struct TestRegister_##name { \
        TestRegister_##name() { \
            register_test(#name, test_func_##name); \
        } \
    } g_test_register_##name; \
    void test_func_##name()

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "  Assertion failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            throw std::runtime_error("Assertion failed: " #expr); \
        } \
    } while (0)

#define ASSERT_FALSE(expr) \
    do { \
        if (expr) { \
            std::cerr << "  Assertion failed: !" << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            throw std::runtime_error("Assertion failed: !" #expr); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "  Assertion failed: " << #a << " == " << #b \
                      << " (Value " << (a) << " != " << (b) << ") at " \
                      << __FILE__ << ":" << __LINE__ << std::endl; \
            throw std::runtime_error("Assertion failed: EQ check"); \
        } \
    } while (0)

#define ASSERT_DOUBLE_EQ(a, b) \
    do { \
        if (std::abs((a) - (b)) > 1e-9) { \
            std::cerr << "  Assertion failed: " << #a << " == " << #b \
                      << " (Value " << (a) << " != " << (b) << ") at " \
                      << __FILE__ << ":" << __LINE__ << std::endl; \
            throw std::runtime_error("Assertion failed: Double EQ check"); \
        } \
    } while (0)
