#include "types.hpp"
#include <limits>

namespace {

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

// value = value * 10 + digit, failing on int64 overflow
bool push_digit(int64_t& value, int digit) {
    if (value > (kMax - digit) / 10) {
        return false;
    }
    value = value * 10 + digit;
    return true;
}

} // namespace

std::optional<int64_t> parse_fixed(std::string_view text) {
    size_t i = 0;
    if (i < text.size() && text[i] == '+') {
        ++i;
    }

    // Mantissa: digits with at most one decimal point
    std::string digits;
    int fraction_digits = 0;
    bool seen_point = false;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
            fraction_digits += seen_point;
        } else if (c == '.' && !seen_point) {
            seen_point = true;
        } else {
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    // Optional exponent
    int exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            negative = text[i] == '-';
            ++i;
        }
        if (i == text.size()) {
            return std::nullopt;
        }
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            exponent = exponent * 10 + (text[i] - '0');
            if (exponent > 100) {
                return std::nullopt;
            }
        }
        exponent = negative ? -exponent : exponent;
    }
    if (i != text.size()) {
        return std::nullopt;
    }

    // units = digits * 10^shift
    int shift = kFixedDecimals + exponent - fraction_digits;
    if (shift < 0) {
        // The dropped digits must all be zero, otherwise the value is finer than 1e-8
        size_t keep = digits.size() > static_cast<size_t>(-shift) ? digits.size() - static_cast<size_t>(-shift) : 0;
        for (size_t k = keep; k < digits.size(); ++k) {
            if (digits[k] != '0') {
                return std::nullopt;
            }
        }
        digits.resize(keep);
        shift = 0;
    }

    int64_t value = 0;
    for (char c : digits) {
        if (!push_digit(value, c - '0')) {
            return std::nullopt;
        }
    }
    for (int k = 0; k < shift; ++k) {
        if (!push_digit(value, 0)) {
            return std::nullopt;
        }
    }
    return value;
}

std::string format_fixed(int64_t value) {
    std::string sign;
    uint64_t magnitude = static_cast<uint64_t>(value);
    if (value < 0) {
        sign = "-";
        magnitude = ~magnitude + 1;
    }

    std::string fraction = std::to_string(magnitude % kFixedScale);
    fraction.insert(0, kFixedDecimals - fraction.size(), '0');
    while (fraction.size() > 1 && fraction.back() == '0') {
        fraction.pop_back();
    }
    return sign + std::to_string(magnitude / kFixedScale) + "." + fraction;
}
