#include "fees.hpp"
#include "int128.hpp"
#include <limits>

namespace {

constexpr uint64_t kMax = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

uint64_t to_u64_saturated(const int128::U128& v) {
    return (v.hi != 0 || v.lo > kMax) ? kMax : v.lo;
}

} // namespace

int64_t fee_for(Price price, Quantity quantity, int64_t rate) {
    if (rate == 0 || price <= 0 || quantity <= 0) {
        return 0;
    }
    constexpr uint32_t scale = static_cast<uint32_t>(kFixedScale);

    // price (1e-8) x quantity (1e-8) is in 1e-16 units; back to 1e-8 quote units
    uint64_t notional = to_u64_saturated(
        int128::div_round(int128::mul(static_cast<uint64_t>(price), static_cast<uint64_t>(quantity)), scale));

    // notional (1e-8) x |rate| (1e-8) is in 1e-16 units; back to 1e-8, then apply the sign
    uint64_t magnitude = rate < 0 ? static_cast<uint64_t>(-(rate + 1)) + 1 : static_cast<uint64_t>(rate);
    uint64_t fee = to_u64_saturated(int128::div_round(int128::mul(notional, magnitude), scale));
    return rate < 0 ? -static_cast<int64_t>(fee) : static_cast<int64_t>(fee);
}
