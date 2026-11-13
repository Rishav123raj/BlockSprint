#pragma once
// Observed book (P3.9, architecture §10): a strategy's own L2 view of the market, built from the data's
// book batches as they are DELIVERED to the strategy, i.e. after feed latency. A strategy reads only this,
// never the engine's book. It shows the market's data: not the strategy's own orders, and not the impact of
// its taker fills (the live platform's strategies see the same thing: the exchange feed).
//
// Batches are applied with the reconstruction rules (snapshot replaces, depth sets levels, gap clears and
// invalidates until the next snapshot), so after the same batches it equals the Reconstructor's book.
// Prices and quantities come out in 1e-8 units, like everything the strategy sends. With permanent impact
// (P6.6) every price is moved by the batch's shift, as the market moved at the exchange when it was applied.
#include "l2_book.hpp"
#include "replayer.hpp"

#include "models.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace sim {

struct BookLevel {
    Price price;    // 1e-8 units
    Quantity qty;   // 1e-8 units
    bool operator==(const BookLevel& o) const { return price == o.price && qty == o.qty; }
};

// A trade of the data, as the strategy sees it
struct TradeEvent {
    int64_t ts_local;   // when the recorder received it (the simulation's exchange time)
    int64_t ts_exch;    // Binance's timestamp
    Price price;
    Quantity qty;
    Side aggressor;     // BUY: a buyer lifted the ask
};

class ObservedBook {
public:
    ObservedBook(int64_t tick_size, int64_t lot_size);

    // Applies a batch delivered at sim time seen_at. DEPTH, SNAPSHOT and GAP change the book; a TRADE
    // only updates last_trade().
    void apply(const Batch& batch, uint64_t seen_at);

    // A snapshot has established the book and no gap has happened since
    bool valid() const { return valid_; }

    std::optional<BookLevel> best_bid() const;
    std::optional<BookLevel> best_ask() const;
    // Quantity at a price (0 if none, or if the price is off the tick grid)
    Quantity qty(Side side, Price price) const;
    // The best n levels of a side, best first
    void top(Side side, size_t n, std::vector<BookLevel>& out) const;
    size_t levels(Side side) const { return book_.levels(side == Side::BUY ? 0 : 1); }

    std::optional<TradeEvent> last_trade() const { return last_trade_; }

    // ts_local of the newest batch applied (the market time the view shows)
    int64_t data_time() const { return data_time_; }
    // Sim time the newest batch reached the strategy (data_time() + feed latency)
    uint64_t seen_at() const { return seen_at_; }
    uint64_t batches() const { return batches_; }

    int64_t tick_size() const { return tick_; }
    int64_t lot_size() const { return lot_; }

    // The data trade in a TRADE record, in 1e-8 units
    static TradeEvent trade_event(const bse::Record& r, int64_t tick_size, int64_t lot_size);

private:
    std::optional<BookLevel> to_level(const std::optional<Level>& l) const;

    int64_t tick_;
    int64_t lot_;
    int64_t shift_ = 0;  // ticks, from the newest batch
    L2Book book_;
    bool valid_ = false;
    std::optional<TradeEvent> last_trade_;
    int64_t data_time_ = 0;
    uint64_t seen_at_ = 0;
    uint64_t batches_ = 0;
    mutable std::vector<Level> scratch_;
};

} // namespace sim
