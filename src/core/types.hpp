#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Prices and quantities are fixed-point integers in units of 1e-8, the finest precision Binance uses
// for any spot symbol. All matching arithmetic is exact integer arithmetic: no level splitting from
// floating-point rounding (defect D5). Per-symbol tick and lot sizes are validation rules (SymbolSpec, P1.12).
using Price = int64_t;
using Quantity = int64_t;

constexpr int kFixedDecimals = 8;
constexpr int64_t kFixedScale = 100000000;

// Exact parse of a decimal string ("101", "0.5", "68000.01000000", "1e-05") into fixed-point units.
// Returns nullopt if the text is malformed, negative, finer than 1e-8, or out of range.
std::optional<int64_t> parse_fixed(std::string_view text);

// Exact decimal rendering with trailing zeros trimmed, keeping at least one decimal ("101.5", "7.0").
std::string format_fixed(int64_t value);

// 1.0 wire format for prices and quantities: std::to_string of the double value (6 decimals, "%f").
// Kept for API compatibility until JSON moves to the API layer (P1.11).
inline std::string format_legacy(int64_t value) {
    return std::to_string(static_cast<double>(value) / kFixedScale);
}

// Lossy conversions for display and for callers that hold binary floating-point values (tests, Python).
inline double to_double(int64_t value) { return static_cast<double>(value) / kFixedScale; }
inline int64_t from_double(double value) { return std::llround(value * kFixedScale); }
