#pragma once
#include <cstdint>

// Minimal unsigned 128-bit arithmetic for exact fixed-point products (MSVC has no __int128).
// Used by min-notional checks (symbol_spec) and fee calculation (fees).
namespace int128 {

struct U128 {
    uint64_t hi = 0;
    uint64_t lo = 0;
};

inline U128 mul(uint64_t a, uint64_t b) {
    uint64_t a_lo = a & 0xffffffffULL, a_hi = a >> 32;
    uint64_t b_lo = b & 0xffffffffULL, b_hi = b >> 32;
    uint64_t lo_lo = a_lo * b_lo;
    uint64_t hi_lo = a_hi * b_lo;
    uint64_t lo_hi = a_lo * b_hi;
    uint64_t hi_hi = a_hi * b_hi;
    uint64_t cross = (lo_lo >> 32) + (hi_lo & 0xffffffffULL) + lo_hi;
    return {hi_hi + (hi_lo >> 32) + (cross >> 32), (cross << 32) | (lo_lo & 0xffffffffULL)};
}

inline bool less(const U128& a, const U128& b) {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}

// a / divisor, rounded half up. divisor must be non-zero and fit in 32 bits.
inline U128 div_round(const U128& a, uint32_t divisor) {
    uint32_t limbs[4] = {static_cast<uint32_t>(a.hi >> 32), static_cast<uint32_t>(a.hi),
                         static_cast<uint32_t>(a.lo >> 32), static_cast<uint32_t>(a.lo)};
    uint64_t rem = 0;
    for (uint32_t& limb : limbs) {
        uint64_t cur = (rem << 32) | limb;
        limb = static_cast<uint32_t>(cur / divisor);
        rem = cur % divisor;
    }
    U128 q{(static_cast<uint64_t>(limbs[0]) << 32) | limbs[1], (static_cast<uint64_t>(limbs[2]) << 32) | limbs[3]};
    if (2 * rem >= divisor) {
        q.lo += 1;
        q.hi += q.lo == 0;
    }
    return q;
}

} // namespace int128
