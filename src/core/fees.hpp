#pragma once
#include "types.hpp"
#include <optional>
#include <string>
#include <unordered_map>

// Trading fees (P1.13).
//
// Rates are fixed-point fractions in 1e-8 units: 0.001 (0.10%) = 100000. A negative rate is a rebate.
// Fees are charged in the quote currency (USDT for BTC-USDT) on the fill's notional (price x quantity).
// Binance charges a buyer's fee in the base asset unless BNB is used; expressing every fee in quote
// terms makes PnL directly comparable, and equals Binance's charge valued at the fill price.
struct FeeSchedule {
    int64_t maker_rate = 0;
    int64_t taker_rate = 0;
};

// Binance spot, regular (VIP 0) tier: 0.10% maker, 0.10% taker. Paying fees in BNB takes 25% off.
inline FeeSchedule binance_spot_regular() { return {100000, 100000}; }

// Fee for one fill, in quote units of 1e-8, exact: the notional is rounded to 1e-8, then the fee is
// rounded to 1e-8 (both half away from zero). Never more than 1e-8 off the unrounded value.
// Exact for fill notionals up to ~92 billion quote units (int64 in 1e-8 units); larger ones saturate.
int64_t fee_for(Price price, Quantity quantity, int64_t rate);

// Fee schedule per participant (order owner), with a default for everyone else.
class FeeBook {
public:
    void set_default(const FeeSchedule& schedule) { default_ = schedule; }
    void set(const std::string& owner, const FeeSchedule& schedule) { by_owner_[owner] = schedule; }
    void clear(const std::string& owner) { by_owner_.erase(owner); }

    const FeeSchedule& default_schedule() const { return default_; }

    // The owner's own schedule, if one was set
    std::optional<FeeSchedule> own_schedule(const std::string& owner) const {
        auto it = by_owner_.find(owner);
        return it == by_owner_.end() ? std::nullopt : std::optional<FeeSchedule>(it->second);
    }

    // The schedule that applies to the owner's fills
    const FeeSchedule& schedule_for(const std::string& owner) const {
        auto it = by_owner_.find(owner);
        return it == by_owner_.end() ? default_ : it->second;
    }

private:
    FeeSchedule default_;
    std::unordered_map<std::string, FeeSchedule> by_owner_;
};
