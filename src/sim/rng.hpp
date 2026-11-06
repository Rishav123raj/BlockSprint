#pragma once
// Deterministic random numbers for the simulator (P3.2).
//
// The same seed gives the same sequence on every compiler and platform. std::*_distribution can't be used:
// MSVC and libstdc++ implement them differently. Neither can std::log / std::exp / std::cos: the C++
// standard doesn't require them to be correctly rounded, so they may differ in the last bit between
// libraries. Everything here uses only + - * / and sqrt, which IEEE 754 requires to be exactly rounded.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sim {

// SplitMix64: expands one 64-bit seed into the generator's state
inline uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Natural log from + - * / only: x = m * 2^e with m in [sqrt(1/2), sqrt(2)), then
// log(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| < 0.172, as a series. Relative error < 1e-15.
inline double det_log(double x) {
    if (!(x > 0.0)) {
        throw std::domain_error("det_log: argument must be positive");
    }
    int e = 0;
    double m = std::frexp(x, &e);  // exact: x = m * 2^e, m in [0.5, 1)
    if (m < 0.70710678118654752440) {
        m *= 2.0;
        --e;
    }
    const double s = (m - 1.0) / (m + 1.0);
    const double s2 = s * s;
    double term = s;
    double sum = 0.0;
    for (int k = 1; k <= 41; k += 2) {
        sum += term / k;
        term *= s2;
    }
    return 2.0 * sum + e * 0.69314718055994530942;
}

// xoshiro256** (Blackman & Vigna): fast, 256-bit state, passes BigCrush
class Rng {
public:
    explicit Rng(uint64_t seed) {
        uint64_t sm = seed;
        for (auto& word : s_) {
            word = splitmix64(sm);
        }
    }

    uint64_t next() {
        const uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, 1), 53 random bits
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

    // Uniform integer in [0, n), unbiased (rejection sampling); n > 0
    uint64_t below(uint64_t n) {
        if (n == 0) {
            throw std::invalid_argument("Rng::below: n must be positive");
        }
        const uint64_t threshold = (0 - n) % n;  // 2^64 mod n
        for (;;) {
            const uint64_t r = next();
            if (r >= threshold) {
                return r % n;
            }
        }
    }

    // Exponential with the given mean
    double exponential(double mean) { return -mean * det_log(1.0 - uniform()); }

    // Standard normal (Marsaglia polar method: needs only log and sqrt)
    double normal() {
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }
        for (;;) {
            const double u = 2.0 * uniform() - 1.0;
            const double v = 2.0 * uniform() - 1.0;
            const double q = u * u + v * v;
            if (q > 0.0 && q < 1.0) {
                const double f = std::sqrt(-2.0 * det_log(q) / q);
                spare_ = v * f;
                has_spare_ = true;
                return u * f;
            }
        }
    }

    double normal(double mean, double sd) { return mean + sd * normal(); }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    uint64_t s_[4]{};
    double spare_ = 0.0;
    bool has_spare_ = false;
};

// Inverse-CDF sampling from observed values (e.g. recorded latencies): sorts them once, then a uniform
// draw picks a position and interpolates linearly between neighbours
class EmpiricalDistribution {
public:
    explicit EmpiricalDistribution(std::vector<double> values) : sorted_(std::move(values)) {
        if (sorted_.empty()) {
            throw std::invalid_argument("EmpiricalDistribution: no values");
        }
        std::sort(sorted_.begin(), sorted_.end());
    }

    double quantile(double q) const {
        if (sorted_.size() == 1) {
            return sorted_[0];
        }
        const double pos = q * static_cast<double>(sorted_.size() - 1);
        const size_t i = static_cast<size_t>(pos);
        if (i + 1 >= sorted_.size()) {
            return sorted_.back();
        }
        const double frac = pos - static_cast<double>(i);
        return sorted_[i] + frac * (sorted_[i + 1] - sorted_[i]);
    }

    double sample(Rng& rng) const { return quantile(rng.uniform()); }

    size_t size() const { return sorted_.size(); }

private:
    std::vector<double> sorted_;
};

} // namespace sim
