#pragma once
// ImpactOverlay (P6.1, architecture §8.3): how long a strategy's taker fills keep liquidity out of the book.
//
// The data keeps showing liquidity that a strategy's taker fill took in the simulation, because the real
// market never saw that fill. The overlay remembers, per data level, how much synthetic quantity strategy
// taker fills consumed, decaying with half-life tau:
//
//   outstanding(t) = sum of each take q_i * 2^(-(t - t_i) / tau), floored to whole lots
//
// When the data next changes the level, the synthesizer drives it towards (data - outstanding) instead of
// the data, so the taken quantity comes back gradually rather than all at once. Between data changes the
// level is left alone, as with tau = 0.
//
//   tau = 0     disabled: the level is set back to the data on its next change (the P3 behaviour)
//   reset       a level's entry goes when the data empties the level (that liquidity is gone, and new
//               orders there owe nothing to it); every entry goes on a snapshot or a gap
//
// Prices are data ticks, quantities 1e-8 units; sides are data sides (0 bid, 1 ask), the side of the
// resting liquidity that was taken.
//
// PermanentImpact (P6.6, off by default): the square-root law, for execution-algorithm research. The net
// quantity X the strategies took from the market's liquidity (buys +, sells -) moves the whole market by
//
//   shift / price = sign(X) * Y * sigma * sqrt(|X| / V)
//
// Y the coefficient (of order 1), sigma the daily volatility, V the daily volume (base units). The shift is
// rounded to whole ticks at the price of the latest take, and doesn't decay: selling back what was bought
// brings it back to 0. The synthesizer moves every data level by it.
#include "models.hpp"

#include <cstdint>
#include <unordered_map>

namespace sim {

struct PermanentImpactConfig {
    double coefficient = 0;       // Y; 0 disables
    double daily_volatility = 0;  // sigma, e.g. 0.025 for 2.5% a day
    int64_t daily_volume = 0;     // V, 1e-8 base units
    bool enabled() const { return coefficient > 0; }
};

class PermanentImpact {
public:
    // Throws std::invalid_argument when enabled without a positive volatility, volume and tick size
    PermanentImpact(PermanentImpactConfig config, int64_t tick_size);

    bool enabled() const { return config_.enabled(); }
    const PermanentImpactConfig& config() const { return config_; }

    // A strategy took qty (1e-8 units) of the market's liquidity at price as the aggressor; returns the new
    // shift in ticks
    int64_t take(Side aggressor, int64_t qty, Price price);

    int64_t net_qty() const { return net_; }      // X, 1e-8 units
    double fraction() const;                      // shift / price
    int64_t shift_ticks() const { return shift_; }
    int64_t max_abs_shift_ticks() const { return max_abs_shift_; }

private:
    PermanentImpactConfig config_;
    int64_t tick_;
    int64_t net_ = 0;
    int64_t shift_ = 0;
    int64_t max_abs_shift_ = 0;
};

class ImpactOverlay {
public:
    // half_life_ns 0 disables the overlay; lot_size (1e-8 units) is what outstanding amounts are floored to
    ImpactOverlay(uint64_t half_life_ns, int64_t lot_size);

    bool enabled() const { return half_life_ns_ > 0; }
    uint64_t half_life_ns() const { return half_life_ns_; }

    // A strategy taker fill took qty of synthetic liquidity at (side, px) at time now
    void consume(uint8_t side, int64_t px, int64_t qty, uint64_t now);
    // What is still held back at (side, px) at time now, in whole lots; drops an entry below one lot
    int64_t outstanding(uint8_t side, int64_t px, uint64_t now);
    // The data emptied the level
    void reset(uint8_t side, int64_t px) { levels_.erase(key(side, px)); }
    // A snapshot or gap
    void clear() { levels_.clear(); }

    size_t levels() const { return levels_.size(); }

private:
    struct Entry {
        double qty = 0;    // as of t
        uint64_t t = 0;
    };

    static uint64_t key(uint8_t side, int64_t px) {
        return (static_cast<uint64_t>(side) << 63) | (static_cast<uint64_t>(px) & ~(uint64_t{1} << 63));
    }
    double decayed(const Entry& e, uint64_t now) const;

    uint64_t half_life_ns_;
    int64_t lot_;
    std::unordered_map<uint64_t, Entry> levels_;
};

} // namespace sim
