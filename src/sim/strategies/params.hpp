#pragma once
// Strategy parameters from the run config's strategy.params object (P3.14). Each getter reads one key, with
// a default when the key is absent; done() rejects keys no getter asked for, so a misspelt parameter is an
// error rather than a silently ignored setting. Errors name the key: "strategy.params.size: ...".
#include "types.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

namespace sim {

class StrategyParams {
public:
    StrategyParams(const nlohmann::json& params, std::string strategy) : p_(params), strategy_(std::move(strategy)) {
        if (!p_.is_null() && !p_.is_object()) {
            throw std::invalid_argument("strategy.params: expected an object for '" + strategy_ + "'");
        }
    }

    // An exact positive decimal (quantity, price) as a string such as "0.001", or a number
    int64_t decimal(const std::string& key, const std::string& fallback) {
        const nlohmann::json* v = find(key);
        std::string text = fallback;
        if (v) {
            if (v->is_string()) {
                text = v->get<std::string>();
            } else if (v->is_number()) {
                text = v->dump();
            } else {
                bad(key, "expected a decimal such as \"0.001\", got " + v->dump());
            }
        }
        const auto parsed = parse_fixed(text);
        if (!parsed || *parsed <= 0) bad(key, "expected a positive decimal with at most 8 places, got " + text);
        return *parsed;
    }

    uint64_t integer(const std::string& key, uint64_t fallback) {
        const nlohmann::json* v = find(key);
        if (!v) return fallback;
        if (!v->is_number_unsigned() && !(v->is_number_integer() && v->get<int64_t>() >= 0)) {
            bad(key, "expected a non-negative integer, got " + v->dump());
        }
        return v->get<uint64_t>();
    }

    // Milliseconds (may be fractional) as ns
    uint64_t millis(const std::string& key, double fallback_ms) {
        const nlohmann::json* v = find(key);
        double ms = fallback_ms;
        if (v) {
            if (!v->is_number() || !(v->get<double>() >= 0.0)) bad(key, "expected milliseconds >= 0, got " + v->dump());
            ms = v->get<double>();
        }
        return static_cast<uint64_t>(std::llround(ms * 1e6));
    }

    std::string text(const std::string& key, const std::string& fallback) {
        const nlohmann::json* v = find(key);
        if (!v) return fallback;
        if (!v->is_string()) bad(key, "expected a string, got " + v->dump());
        return v->get<std::string>();
    }

    bool flag(const std::string& key, bool fallback) {
        const nlohmann::json* v = find(key);
        if (!v) return fallback;
        if (!v->is_boolean()) bad(key, "expected true or false, got " + v->dump());
        return v->get<bool>();
    }

    // Call after reading every parameter
    void done() const {
        if (!p_.is_object()) return;
        for (const auto& [key, value] : p_.items()) {
            if (!asked_.count(key)) {
                std::string known;
                for (const auto& k : asked_) known += (known.empty() ? "" : ", ") + k;
                throw std::invalid_argument("strategy.params." + key + ": unknown parameter for '" + strategy_ + "'" +
                                            (known.empty() ? " (it takes none)" : " (" + known + ")"));
            }
        }
    }

    [[noreturn]] void bad(const std::string& key, const std::string& what) const {
        throw std::invalid_argument("strategy.params." + key + ": " + what);
    }

private:
    const nlohmann::json* find(const std::string& key) {
        asked_.insert(key);
        if (!p_.is_object() || !p_.contains(key)) return nullptr;
        return &p_[key];
    }

    nlohmann::json p_;
    std::string strategy_;
    std::set<std::string> asked_;
};

} // namespace sim
