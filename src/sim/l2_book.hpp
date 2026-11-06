#pragma once
// A pure L2 book: quantity per price level, in the .bse file's ticks and lots (P3.5). It holds exactly
// what the data says, with no strategy orders, and is the reference the synthesizer is checked against.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace sim {

struct Level {
    int64_t px;
    int64_t qty;
    bool operator==(const Level& o) const { return px == o.px && qty == o.qty; }
};

class L2Book {
public:
    void clear() {
        bids_.clear();
        asks_.clear();
    }

    // Sets a level's quantity; 0 removes the level. side: 0 bid, 1 ask
    void set(uint8_t side, int64_t px, int64_t qty) {
        if (side == 0) {
            set_in(bids_, px, qty);
        } else {
            set_in(asks_, px, qty);
        }
    }

    int64_t qty(uint8_t side, int64_t px) const {
        if (side == 0) {
            auto it = bids_.find(px);
            return it == bids_.end() ? 0 : it->second;
        }
        auto it = asks_.find(px);
        return it == asks_.end() ? 0 : it->second;
    }

    std::optional<Level> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Level>(Level{bids_.begin()->first, bids_.begin()->second});
    }

    std::optional<Level> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Level>(Level{asks_.begin()->first, asks_.begin()->second});
    }

    // Best bid at or above best ask (a locked book counts)
    bool crossed() const { return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first; }

    // The best n levels of one side, best first
    void top(uint8_t side, size_t n, std::vector<Level>& out) const {
        out.clear();
        if (side == 0) {
            copy_top(bids_, n, out);
        } else {
            copy_top(asks_, n, out);
        }
    }

    size_t levels(uint8_t side) const { return side == 0 ? bids_.size() : asks_.size(); }

private:
    template <typename Map>
    static void set_in(Map& m, int64_t px, int64_t qty) {
        if (qty == 0) {
            m.erase(px);
        } else {
            m[px] = qty;
        }
    }

    template <typename Map>
    static void copy_top(const Map& m, size_t n, std::vector<Level>& out) {
        for (auto it = m.begin(); it != m.end() && out.size() < n; ++it) {
            out.push_back(Level{it->first, it->second});
        }
    }

    std::map<int64_t, int64_t, std::greater<int64_t>> bids_;  // best (highest) first
    std::map<int64_t, int64_t> asks_;                         // best (lowest) first
};

} // namespace sim
