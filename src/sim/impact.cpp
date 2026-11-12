#include "impact.hpp"

#include <cmath>
#include <stdexcept>

namespace sim {

ImpactOverlay::ImpactOverlay(uint64_t half_life_ns, int64_t lot_size) : half_life_ns_(half_life_ns), lot_(lot_size) {
    if (lot_size <= 0) {
        throw std::invalid_argument("ImpactOverlay needs a positive lot size");
    }
}

double ImpactOverlay::decayed(const Entry& e, uint64_t now) const {
    if (now <= e.t) {
        return e.qty;
    }
    return e.qty * std::exp2(-static_cast<double>(now - e.t) / static_cast<double>(half_life_ns_));
}

void ImpactOverlay::consume(uint8_t side, int64_t px, int64_t qty, uint64_t now) {
    if (!enabled() || qty <= 0) {
        return;
    }
    Entry& e = levels_[key(side, px)];
    e.qty = decayed(e, now) + static_cast<double>(qty);
    e.t = now;
}

int64_t ImpactOverlay::outstanding(uint8_t side, int64_t px, uint64_t now) {
    if (levels_.empty()) {
        return 0;
    }
    auto it = levels_.find(key(side, px));
    if (it == levels_.end()) {
        return 0;
    }
    // A hair under a whole lot counts as that lot: 2^-k rounding must not lose a lot that hasn't decayed
    const double lots = std::floor(decayed(it->second, now) / static_cast<double>(lot_) + 1e-9);
    if (lots < 1) {
        levels_.erase(it);
        return 0;
    }
    return static_cast<int64_t>(lots) * lot_;
}

} // namespace sim
