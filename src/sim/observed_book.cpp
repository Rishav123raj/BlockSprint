#include "observed_book.hpp"

#include <stdexcept>

namespace sim {

ObservedBook::ObservedBook(int64_t tick_size, int64_t lot_size) : tick_(tick_size), lot_(lot_size) {
    if (tick_size <= 0 || lot_size <= 0) {
        throw std::invalid_argument("ObservedBook needs positive tick and lot sizes");
    }
}

TradeEvent ObservedBook::trade_event(const bse::Record& r, int64_t tick_size, int64_t lot_size) {
    return TradeEvent{r.ts_local, r.ts_exch, r.px * tick_size, r.qty * lot_size,
                      r.side == bse::BID ? Side::BUY : Side::SELL};
}

void ObservedBook::apply(const Batch& batch, uint64_t seen_at) {
    ++batches_;
    data_time_ = batch.ts_local();
    seen_at_ = seen_at;
    shift_ = batch.shift;
    switch (batch.kind) {
        case BatchKind::TRADE:
            last_trade_ = trade_event(*batch.begin, tick_, lot_);
            last_trade_->price += shift_ * tick_;
            break;
        case BatchKind::GAP:
            book_.clear();
            valid_ = false;
            break;
        case BatchKind::SNAPSHOT:
            book_.clear();
            for (const bse::Record* r = batch.begin + 1; r + 1 < batch.end; ++r) {
                book_.set(r->side, r->px, r->qty);
            }
            valid_ = true;
            break;
        case BatchKind::DEPTH:
            for (const bse::Record* r = batch.begin; r < batch.end; ++r) {
                book_.set(r->side, r->px, r->qty);
            }
            break;
    }
}

std::optional<BookLevel> ObservedBook::to_level(const std::optional<Level>& l) const {
    if (!l) {
        return std::nullopt;
    }
    return BookLevel{(l->px + shift_) * tick_, l->qty * lot_};
}

std::optional<BookLevel> ObservedBook::best_bid() const { return to_level(book_.best_bid()); }

std::optional<BookLevel> ObservedBook::best_ask() const { return to_level(book_.best_ask()); }

Quantity ObservedBook::qty(Side side, Price price) const {
    if (price % tick_ != 0) {
        return 0;
    }
    return book_.qty(side == Side::BUY ? 0 : 1, price / tick_ - shift_) * lot_;
}

void ObservedBook::top(Side side, size_t n, std::vector<BookLevel>& out) const {
    book_.top(side == Side::BUY ? 0 : 1, n, scratch_);
    out.clear();
    for (const Level& l : scratch_) {
        out.push_back(BookLevel{(l.px + shift_) * tick_, l.qty * lot_});
    }
}

} // namespace sim
