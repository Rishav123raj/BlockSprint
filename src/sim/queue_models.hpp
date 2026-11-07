#pragma once
// Queue models (P3.7). When a price level shrinks and no trade explains it, which part of the level's queue
// was cancelled? The answer decides how fast a strategy order moves to the front (architecture §9).
//
//   cancel_from_back   newest synthetic quantity first: the queue ahead only shrinks through trades (pessimistic)
//   cancel_from_front  oldest first (optimistic)
//   pro_rata           every synthetic order in proportion to its size (neutral)
//   prob_power:n       the part ahead of the first strategy order loses a share a^n / (a^n + b^n) of the
//                      cancel, the part behind the rest (a, b: synthetic quantity ahead and behind, in lots)
//   prob_log           the same with log(1 + x)
//
// The probabilistic models use the expected split rather than a random draw (as hftbacktest does), so they
// need no random numbers; the Rng parameter is there for models that do. Within a part, quantity is taken
// from the back. Without a strategy order in the level, they fall back to cancel_from_back.
#include "rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace sim {

// One resting order of a price level, in queue order (front = oldest = matched first)
struct QueueSlot {
    int64_t qty;     // open quantity (1e-8 units)
    bool synthetic;  // market-data liquidity; strategy orders are never chosen
};

class QueueModel {
public:
    virtual ~QueueModel() = default;
    virtual std::string name() const = 0;

    // Splits a cancel of `amount` over the synthetic slots: take[i] (take is resized to queue.size()) is
    // what slot i loses, 0 for strategy slots and never more than the slot holds. The total is
    // min(amount, synthetic total). Shares are multiples of `unit` (the lot size) where the sizes allow.
    virtual void choose(const std::vector<QueueSlot>& queue, int64_t amount, int64_t unit, Rng& rng,
                        std::vector<int64_t>& take) const = 0;
};

namespace detail {

// Takes up to `amount` from the synthetic slots in [first, last), walking from the back or the front, on
// top of what take already holds. Returns the part that didn't fit.
inline int64_t take_in_order(const std::vector<QueueSlot>& queue, size_t first, size_t last, bool from_back,
                             int64_t amount, std::vector<int64_t>& take) {
    for (size_t n = first; n < last && amount > 0; ++n) {
        const size_t i = from_back ? last - 1 - (n - first) : n;
        if (!queue[i].synthetic) {
            continue;
        }
        const int64_t t = std::min(queue[i].qty - take[i], amount);
        take[i] += t;
        amount -= t;
    }
    return amount;
}

inline int64_t synthetic_total(const std::vector<QueueSlot>& queue, size_t first, size_t last) {
    int64_t total = 0;
    for (size_t i = first; i < last; ++i) {
        if (queue[i].synthetic) {
            total += queue[i].qty;
        }
    }
    return total;
}

// Index of the first strategy slot, or queue.size()
inline size_t first_strategy(const std::vector<QueueSlot>& queue) {
    size_t i = 0;
    while (i < queue.size() && queue[i].synthetic) {
        ++i;
    }
    return i;
}

} // namespace detail

class CancelFromBack final : public QueueModel {
public:
    std::string name() const override { return "cancel_from_back"; }
    void choose(const std::vector<QueueSlot>& queue, int64_t amount, int64_t, Rng&,
                std::vector<int64_t>& take) const override {
        take.assign(queue.size(), 0);
        detail::take_in_order(queue, 0, queue.size(), true, amount, take);
    }
};

class CancelFromFront final : public QueueModel {
public:
    std::string name() const override { return "cancel_from_front"; }
    void choose(const std::vector<QueueSlot>& queue, int64_t amount, int64_t, Rng&,
                std::vector<int64_t>& take) const override {
        take.assign(queue.size(), 0);
        detail::take_in_order(queue, 0, queue.size(), false, amount, take);
    }
};

// Proportional shares in whole lots by largest remainder (ties go to the newer slot); any quantity that
// isn't a whole lot is then taken from the back
class ProRata final : public QueueModel {
public:
    std::string name() const override { return "pro_rata"; }
    void choose(const std::vector<QueueSlot>& queue, int64_t amount, int64_t unit, Rng&,
                std::vector<int64_t>& take) const override {
        take.assign(queue.size(), 0);
        const int64_t total = detail::synthetic_total(queue, 0, queue.size());
        amount = std::min(amount, total);
        if (amount <= 0) {
            return;
        }
        unit = std::max<int64_t>(unit, 1);
        int64_t total_units = 0;
        for (const auto& slot : queue) {
            total_units += slot.synthetic ? slot.qty / unit : 0;
        }
        const int64_t units = std::min(amount / unit, total_units);
        if (units > 0) {
            std::vector<double> remainder(queue.size(), -1.0);
            int64_t given = 0;
            for (size_t i = 0; i < queue.size(); ++i) {
                if (!queue[i].synthetic) {
                    continue;
                }
                const double exact = static_cast<double>(units) * static_cast<double>(queue[i].qty / unit) /
                                     static_cast<double>(total_units);
                const int64_t share = std::min(static_cast<int64_t>(std::floor(exact)), queue[i].qty / unit);
                take[i] = share * unit;
                given += share;
                remainder[i] = exact - static_cast<double>(share);
            }
            for (; given < units; ++given) {
                size_t best = queue.size();
                for (size_t i = 0; i < queue.size(); ++i) {
                    if (queue[i].synthetic && take[i] + unit <= queue[i].qty &&
                        (best == queue.size() || remainder[i] >= remainder[best])) {
                        best = i;
                    }
                }
                if (best == queue.size()) {
                    break;
                }
                take[best] += unit;
                remainder[best] = -1.0;
            }
        }
        int64_t taken = 0;
        for (int64_t t : take) {
            taken += t;
        }
        detail::take_in_order(queue, 0, queue.size(), true, amount - taken, take);
    }
};

// prob_power:n and prob_log
class ProbQueue final : public QueueModel {
public:
    // power > 0: f(x) = x^power; power == 0: f(x) = log(1 + x)
    explicit ProbQueue(double power) : power_(power) {
        if (power < 0.0 || power != power) {
            throw std::invalid_argument("prob_power: n must be positive");
        }
    }

    std::string name() const override {
        if (power_ == 0.0) {
            return "prob_log";
        }
        std::string n = std::to_string(power_);
        n.erase(n.find_last_not_of('0') + 1);
        if (!n.empty() && n.back() == '.') {
            n.pop_back();
        }
        return "prob_power:" + n;
    }

    // Share of a cancel that falls ahead of the strategy order, given the synthetic lots ahead and behind
    double ahead_share(double ahead, double behind) const {
        if (ahead <= 0.0) {
            return 0.0;
        }
        if (behind <= 0.0) {
            return 1.0;
        }
        if (power_ == 0.0) {
            const double fa = det_log(1.0 + ahead);
            const double fb = det_log(1.0 + behind);
            return fa / (fa + fb);
        }
        return 1.0 / (1.0 + det_pow(behind / ahead, power_));
    }

    void choose(const std::vector<QueueSlot>& queue, int64_t amount, int64_t unit, Rng&,
                std::vector<int64_t>& take) const override {
        take.assign(queue.size(), 0);
        const size_t k = detail::first_strategy(queue);
        if (k == queue.size()) {
            detail::take_in_order(queue, 0, queue.size(), true, amount, take);
            return;
        }
        unit = std::max<int64_t>(unit, 1);
        const int64_t a = detail::synthetic_total(queue, 0, k);
        const int64_t b = detail::synthetic_total(queue, k + 1, queue.size());
        amount = std::min(amount, a + b);
        const double share = ahead_share(static_cast<double>(a) / unit, static_cast<double>(b) / unit);
        int64_t ahead = static_cast<int64_t>(std::floor(static_cast<double>(amount / unit) * share + 0.5)) * unit;
        ahead = std::clamp<int64_t>(ahead, 0, std::min(a, amount));
        // Whatever doesn't fit behind is taken ahead, and the other way round
        const int64_t overflow = detail::take_in_order(queue, k + 1, queue.size(), true, amount - ahead, take);
        const int64_t left = detail::take_in_order(queue, 0, k, true, ahead + overflow, take);
        detail::take_in_order(queue, k + 1, queue.size(), true, left, take);
    }

private:
    double power_;
};

// "cancel_from_back", "cancel_from_front", "pro_rata", "prob_power:<n>" (n > 0), "prob_log"
inline std::unique_ptr<QueueModel> make_queue_model(const std::string& spec) {
    if (spec == "cancel_from_back") {
        return std::make_unique<CancelFromBack>();
    }
    if (spec == "cancel_from_front") {
        return std::make_unique<CancelFromFront>();
    }
    if (spec == "pro_rata") {
        return std::make_unique<ProRata>();
    }
    if (spec == "prob_log") {
        return std::make_unique<ProbQueue>(0.0);
    }
    const std::string prefix = "prob_power:";
    if (spec.compare(0, prefix.size(), prefix) == 0) {
        size_t used = 0;
        double n = 0.0;
        try {
            n = std::stod(spec.substr(prefix.size()), &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used == 0 || used != spec.size() - prefix.size() || !(n > 0.0)) {
            throw std::invalid_argument("queue model '" + spec + "': n must be a positive number");
        }
        return std::make_unique<ProbQueue>(n);
    }
    throw std::invalid_argument("unknown queue model '" + spec +
                                "' (cancel_from_back, cancel_from_front, pro_rata, prob_power:<n>, prob_log)");
}

} // namespace sim
