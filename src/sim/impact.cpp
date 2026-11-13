#include "impact.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sim {

PermanentImpact::PermanentImpact(PermanentImpactConfig config, int64_t tick_size)
    : config_(config), tick_(tick_size) {
    if (config_.coefficient < 0) {
        throw std::invalid_argument("PermanentImpact: the coefficient must not be negative");
    }
    if (enabled() && (config_.daily_volatility <= 0 || config_.daily_volume <= 0 || tick_size <= 0)) {
        throw std::invalid_argument("PermanentImpact needs a positive daily volatility, daily volume and tick size");
    }
}

double PermanentImpact::fraction() const {
    if (!enabled() || net_ == 0) {
        return 0;
    }
    const double x = static_cast<double>(net_ < 0 ? -net_ : net_);
    const double f = config_.coefficient * config_.daily_volatility *
                     std::sqrt(x / static_cast<double>(config_.daily_volume));
    return net_ < 0 ? -f : f;
}

int64_t PermanentImpact::take(Side aggressor, int64_t qty, Price price) {
    if (!enabled() || qty <= 0) {
        return shift_;
    }
    net_ += aggressor == Side::BUY ? qty : -qty;
    shift_ = std::llround(fraction() * static_cast<double>(price) / static_cast<double>(tick_));
    max_abs_shift_ = std::max(max_abs_shift_, shift_ < 0 ? -shift_ : shift_);
    return shift_;
}

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
