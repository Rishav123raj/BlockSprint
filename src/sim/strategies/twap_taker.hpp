#pragma once
// TwapTaker (P3.15): buys or sells a total quantity in equal MARKET slices over a duration.
//
// The schedule starts at the first moment the strategy may trade (after the warm-up). Slice k of n is due
// at start + k x duration / n (k = 0 .. n-1); by slice k the target is quantity x (k+1) / n, rounded down
// to the lot, and the last slice takes the rest. Each slice sends the difference between the target and
// what is filled or in flight, so quantity a MARKET order could not fill (no liquidity, a gap, a reject)
// is retried with the next slice. After the last slice it keeps retrying any remainder every interval
// until the target is filled or the run ends. While the book is invalid nothing is sent.
//
// Params: side ("buy" | "sell"), quantity ("0.01"), slices (10), duration_ms (600000)
#include "strategy.hpp"

#include <cstdint>
#include <string>

namespace sim {

class StrategyParams;

class TwapTaker final : public Strategy {
public:
    struct Config {
        Side side = Side::BUY;
        Quantity quantity = 1000000;          // 0.01
        uint64_t slices = 10;
        uint64_t duration_ns = 600000000000;  // 10 min
    };

    explicit TwapTaker(Config config) : c_(config) {}
    static Config config_from(StrategyParams& p);

    std::string name() const override { return "twap_taker"; }
    void on_start(Context& ctx) override;
    void on_timer(Context& ctx, uint64_t timer_id) override;
    void on_exec(Context& ctx, const ExecReport& r) override;

    const Config& config() const { return c_; }
    bool started() const { return started_; }
    uint64_t start_ns() const { return start_ns_; }
    Quantity filled() const { return filled_; }
    Quantity in_flight() const { return committed_ - filled_; }

private:
    uint64_t interval() const { return c_.duration_ns / c_.slices; }
    void tick(Context& ctx);
    Quantity target(uint64_t slices_due, Quantity lot) const;

    Config c_;
    bool started_ = false;
    uint64_t start_ns_ = 0;
    uint64_t due_ = 0;          // slices due so far
    Quantity committed_ = 0;    // sent, minus what came back unfilled
    Quantity filled_ = 0;
};

} // namespace sim
