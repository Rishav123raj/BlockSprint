#pragma once
// PassiveQuoter (P3.15): joins the best bid and the best ask with a fixed size, re-quoting on a timer.
//
// Every refresh it looks at the book it sees and its open orders:
//   - one quote per side at the observed best price, as post-only LIMIT_MAKER (or plain LIMIT);
//   - a quote at another price is replaced (losing its queue place, as on Binance);
//   - a side whose next fill would take the position beyond max_position is not quoted (its quote is
//     cancelled);
//   - while the book is invalid (a gap) every quote is cancelled;
//   - a quote with a request in flight is left alone until the reply comes.
// It reacts to nothing else, so its request rate is bounded by the refresh: at most 2 requests per refresh.
//
// Params: size ("0.001"), refresh_ms (500), max_position ("0.01", absolute, base units), post_only (true)
#include "strategy.hpp"

#include <cstdint>
#include <string>

namespace sim {

class StrategyParams;

class PassiveQuoter final : public Strategy {
public:
    struct Config {
        Quantity size = 100000;            // 0.001
        uint64_t refresh_ns = 500000000;   // 500 ms
        Quantity max_position = 1000000;   // 0.01
        bool post_only = true;
    };

    explicit PassiveQuoter(Config config) : c_(config) {}
    static Config config_from(StrategyParams& p);

    std::string name() const override { return "passive_quoter"; }
    void on_start(Context& ctx) override;
    void on_timer(Context& ctx, uint64_t timer_id) override;
    void on_gap(Context& ctx) override;

    const Config& config() const { return c_; }
    uint64_t refreshes() const { return refreshes_; }

private:
    void requote(Context& ctx);
    void quote_side(Context& ctx, Side side, const Price* target);

    Config c_;
    uint64_t refreshes_ = 0;
};

} // namespace sim
