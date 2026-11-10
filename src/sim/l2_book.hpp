#pragma once
// A pure L2 book: quantity per price level, in the .bse file's ticks and lots (P3.5). It holds exactly
// what the data says, with no strategy orders, and is the reference the synthesizer is checked against.
//
// Layout (P3.18): each side keeps a dense window of quantities indexed by tick around its best price, and
// an ordered map for the levels outside it. On BTCUSDT 94.5% of depth updates fall within +-65,536 ticks of
// the market, while Binance's 5,000 levels per side span up to 10 M ticks (stray orders far away), so the
// window takes the traffic at array speed and the map keeps the far levels in little memory. When the best
// price leaves the middle half of the window, the window is moved to centre it (levels change place
// between the window and the map). Integer-only: the same operations give the same book everywhere.
#include <algorithm>
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

namespace detail {

// One side. Better(a, b): a is a better price than b (bids: higher; asks: lower).
template <typename Better>
class L2Side {
public:
    explicit L2Side(int64_t window) : w_(window) {}

    void clear() {
        if (!q_.empty()) std::fill(q_.begin(), q_.end(), 0);
        far_.clear();
        count_ = 0;
        has_best_ = false;
    }

    void set(int64_t px, int64_t qty) {
        if (q_.empty()) {
            if (qty == 0) return;
            q_.assign(static_cast<size_t>(w_), 0);
            base_ = px - w_ / 2;
        }
        if (in_window(px)) {
            int64_t& slot = q_[static_cast<size_t>(px - base_)];
            if (slot == 0 && qty != 0) {
                ++count_;
            } else if (slot != 0 && qty == 0) {
                --count_;
            }
            slot = qty;
            if (qty != 0) {
                if (!has_best_ || Better()(px, best_)) {
                    best_ = px;
                    has_best_ = true;
                }
            } else if (has_best_ && px == best_) {
                find_best_in_window(px);
            }
        } else {
            auto it = far_.find(px);
            if (qty == 0) {
                if (it != far_.end()) {
                    far_.erase(it);
                    --count_;
                }
            } else if (it == far_.end()) {
                far_.emplace(px, qty);
                ++count_;
            } else {
                it->second = qty;
            }
        }
        recentre_if_needed();
    }

    int64_t qty(int64_t px) const {
        if (!q_.empty() && in_window(px)) return q_[static_cast<size_t>(px - base_)];
        auto it = far_.find(px);
        return it == far_.end() ? 0 : it->second;
    }

    std::optional<Level> best() const {
        // The window's best, unless a far level is better (before the window has been moved to it)
        std::optional<Level> b;
        if (has_best_) b = Level{best_, q_[static_cast<size_t>(best_ - base_)]};
        if (!far_.empty() && (!b || Better()(far_.begin()->first, b->px))) b = Level{far_.begin()->first, far_.begin()->second};
        return b;
    }

    // The best n levels, best first
    void top(size_t n, std::vector<Level>& out) const {
        out.clear();
        if (n == 0 || count_ == 0) return;
        // Far levels better than the window, then the window from its best outwards, then the far levels
        // worse than the window
        auto it = far_.begin();
        for (; it != far_.end() && out.size() < n && better_than_window(it->first); ++it) {
            out.push_back(Level{it->first, it->second});
        }
        if (has_best_) {
            const int64_t step = Better()(1, 0) ? -1 : 1;  // towards worse prices
            for (int64_t px = best_; in_window(px) && out.size() < n; px += step) {
                const int64_t qv = q_[static_cast<size_t>(px - base_)];
                if (qv != 0) out.push_back(Level{px, qv});
            }
        }
        for (; it != far_.end() && out.size() < n; ++it) {
            out.push_back(Level{it->first, it->second});
        }
    }

    size_t size() const { return count_; }

private:
    bool in_window(int64_t px) const { return px >= base_ && px < base_ + w_; }
    // Strictly better than every window price
    bool better_than_window(int64_t px) const {
        return Better()(1, 0) ? px >= base_ + w_ : px < base_;
    }

    // The window's best after `from` (the old best) went to zero: the next level towards worse prices
    void find_best_in_window(int64_t from) {
        const int64_t step = Better()(1, 0) ? -1 : 1;
        for (int64_t px = from + step; in_window(px); px += step) {
            if (q_[static_cast<size_t>(px - base_)] != 0) {
                best_ = px;
                return;
            }
        }
        has_best_ = false;
    }

    // Keeps the overall best in the middle half of the window
    void recentre_if_needed() {
        const std::optional<Level> b = best();
        if (!b) return;
        const int64_t lo = base_ + w_ / 4, hi = base_ + w_ - w_ / 4;
        if (b->px >= lo && b->px < hi) return;
        const int64_t new_base = b->px - w_ / 2;
        // Window levels that leave the new window go to the far map
        for (int64_t i = 0; i < w_; ++i) {
            const int64_t qv = q_[static_cast<size_t>(i)];
            if (qv == 0) continue;
            const int64_t px = base_ + i;
            if (px < new_base || px >= new_base + w_) far_.emplace(px, qv);
        }
        std::vector<int64_t> moved(static_cast<size_t>(w_), 0);
        for (int64_t px = std::max(base_, new_base); px < std::min(base_, new_base) + w_; ++px) {
            moved[static_cast<size_t>(px - new_base)] = q_[static_cast<size_t>(px - base_)];
        }
        q_.swap(moved);
        base_ = new_base;
        // Far levels inside the new window come in
        for (auto it = far_.begin(); it != far_.end();) {
            if (in_window(it->first)) {
                q_[static_cast<size_t>(it->first - base_)] = it->second;
                it = far_.erase(it);
            } else {
                ++it;
            }
        }
        // The new window's best: the overall best, which is now inside it
        best_ = b->px;
        has_best_ = true;
    }

    int64_t w_;
    int64_t base_ = 0;
    std::vector<int64_t> q_;  // window: q_[px - base_]; allocated on the first level
    std::map<int64_t, int64_t, Better> far_;  // outside the window, best first
    size_t count_ = 0;        // levels on this side (window + far)
    bool has_best_ = false;   // the window has a level
    int64_t best_ = 0;        // the window's best price
};

} // namespace detail

class L2Book {
public:
    static constexpr int64_t kDefaultWindow = int64_t{1} << 17;  // ticks per side (1 MB of quantities)

    explicit L2Book(int64_t window = kDefaultWindow) : bids_(window), asks_(window) {}

    void clear() {
        bids_.clear();
        asks_.clear();
    }

    // Sets a level's quantity; 0 removes the level. side: 0 bid, 1 ask
    void set(uint8_t side, int64_t px, int64_t qty) {
        if (side == 0) {
            bids_.set(px, qty);
        } else {
            asks_.set(px, qty);
        }
    }

    int64_t qty(uint8_t side, int64_t px) const { return side == 0 ? bids_.qty(px) : asks_.qty(px); }

    std::optional<Level> best_bid() const { return bids_.best(); }
    std::optional<Level> best_ask() const { return asks_.best(); }

    // Best bid at or above best ask (a locked book counts)
    bool crossed() const {
        const auto b = best_bid(), a = best_ask();
        return b && a && b->px >= a->px;
    }

    // The best n levels of one side, best first
    void top(uint8_t side, size_t n, std::vector<Level>& out) const {
        if (side == 0) {
            bids_.top(n, out);
        } else {
            asks_.top(n, out);
        }
    }

    size_t levels(uint8_t side) const { return side == 0 ? bids_.size() : asks_.size(); }

private:
    detail::L2Side<std::greater<int64_t>> bids_;  // best (highest) first
    detail::L2Side<std::less<int64_t>> asks_;     // best (lowest) first
};

} // namespace sim
