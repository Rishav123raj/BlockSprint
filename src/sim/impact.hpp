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
#include <cstdint>
#include <unordered_map>

namespace sim {

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
