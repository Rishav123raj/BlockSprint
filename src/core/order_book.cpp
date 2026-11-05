#include "order_book.hpp"
#include <algorithm>

OrderBook::OrderBook(std::string symbol, const IClock& clock, uint64_t& trade_id_seq, uint64_t& report_seq,
                     const FeeBook& fees)
    : symbol_(std::move(symbol)), clock_(clock), trade_id_seq_(trade_id_seq), report_seq_(report_seq), fees_(fees) {}

ExecReport& OrderBook::report(std::vector<ExecReport>& reports, ExecType type, const Order& order, ExecReason reason) {
    ExecReport r;
    r.seq = ++report_seq_;
    r.timestamp = clock_.now_ns();
    r.type = type;
    r.reason = reason;
    r.order_id = order.order_id;
    r.owner_id = order.owner_id;
    r.symbol = order.symbol;
    r.side = order.side;
    r.order_type = order.order_type;
    r.price = order.price;
    r.quantity = order.quantity;
    r.cum_qty = order.filled_quantity;
    r.leaves_qty = (type == ExecType::CANCELLED || type == ExecType::REJECTED) ? 0 : order.remaining_quantity;
    r.cum_fee = order.fees_paid;
    reports.push_back(std::move(r));
    return reports.back();
}

std::vector<Trade> OrderBook::submit_order(std::shared_ptr<Order> order, std::vector<ExecReport>& reports) {
    order->filled_quantity = 0;
    order->fees_paid = 0;
    return submit_internal(order, reports, ExecType::ACK);
}

std::vector<Trade> OrderBook::submit_internal(std::shared_ptr<Order> order, std::vector<ExecReport>& reports,
                                              ExecType first_report) {
    order->remaining_quantity = order->quantity - order->filled_quantity;
    order->is_cancelled = false;
    order->has_iterator = false;

    if (orders_lookup_.find(order->order_id) != orders_lookup_.end()) {
        report(reports, ExecType::REJECTED, *order, ExecReason::DUPLICATE_ORDER_ID).text =
            "Duplicate order_id '" + order->order_id + "': an order with this ID is resting (use replace)";
        return {};
    }
    // New orders are validated here; a modify validates its new price and quantity before re-submitting
    if (first_report == ExecType::ACK) {
        if (auto error = validate_order(*order, spec())) {
            report(reports, ExecType::REJECTED, *order, error->reason).text = error->text;
            return {};
        }
        if (order->order_type == OrderType::LIMIT_MAKER && would_cross(*order)) {
            report(reports, ExecType::REJECTED, *order, ExecReason::POST_ONLY_WOULD_TAKE).text =
                post_only_text(*order);
            return {};
        }
    }
    report(reports, first_report, *order);

    std::vector<Trade> trades;
    bool taker_cancelled = false;
    ExecReason unfilled = ExecReason::NONE;  // why an unfilled remainder is cancelled, if it is

    switch (order->order_type) {
        case OrderType::LIMIT:
            trades = match_against_book(order, true, taker_cancelled, reports);
            if (order->remaining_quantity > 0 && !taker_cancelled) {
                // A remainder that still crosses the book only arises when self-trade prevention skipped
                // resting orders; resting it would cross the book (1.0 defect D7), so it is cancelled instead
                if (would_cross(*order)) {
                    unfilled = ExecReason::WOULD_CROSS;
                } else {
                    add_resting_order(order);
                }
            }
            break;
        case OrderType::MARKET:
            trades = match_against_book(order, false, taker_cancelled, reports);
            unfilled = ExecReason::NO_LIQUIDITY;
            break;
        case OrderType::IOC:
            trades = match_against_book(order, true, taker_cancelled, reports);
            unfilled = ExecReason::IOC_REMAINDER;
            break;
        case OrderType::LIMIT_MAKER:
            // Checked above (new orders) or in modify (amends): it does not cross, so it only rests
            if (would_cross(*order)) {
                unfilled = ExecReason::WOULD_CROSS;
            } else {
                add_resting_order(order);
            }
            break;
        case OrderType::FOK:
            // All or nothing. The dry run applies the same self-trade rules as the sweep, so a confirmed
            // FOK fills completely; it never rests (1.0 defect D21)
            if (can_fok_be_fully_filled(order)) {
                trades = match_against_book(order, true, taker_cancelled, reports);
            }
            unfilled = ExecReason::FOK_KILL;
            break;
    }

    if (order->remaining_quantity > 0 && (taker_cancelled || unfilled != ExecReason::NONE)) {
        report(reports, ExecType::CANCELLED, *order, taker_cancelled ? ExecReason::STP_TAKER : unfilled);
    }
    return trades;
}

bool OrderBook::amend_order(const std::string& order_id, Quantity new_total, std::optional<Price> new_price,
                            std::vector<Trade>& generated_trades, std::vector<ExecReport>& reports) {
    auto it = orders_lookup_.find(order_id);
    if (it == orders_lookup_.end()) {
        return false;
    }
    const auto& order = it->second;
    Quantity new_open = new_total - order->filled_quantity;
    if (new_open <= 0) {
        // Nothing left to trade: amending to (or below) the filled quantity cancels the rest
        return cancel_order(order_id, reports);
    }
    return modify_order(order_id, new_open, new_price.value_or(order->price), generated_trades, reports);
}

std::string OrderBook::post_only_text(const Order& order) const {
    return "Post-only order '" + order.order_id + "' at " + format_fixed(order.price) +
           " would trade immediately against the best " + (order.side == Side::BUY ? "ask" : "bid");
}

std::vector<Trade> OrderBook::replace_order(std::shared_ptr<Order> order, std::vector<ExecReport>& reports) {
    if (auto old = erase_order(order->order_id)) {
        report(reports, ExecType::CANCELLED, *old, ExecReason::REPLACED);
    }
    return submit_order(order, reports);
}

std::shared_ptr<Order> OrderBook::erase_order(const std::string& order_id) {
    auto it = orders_lookup_.find(order_id);
    if (it == orders_lookup_.end()) {
        return nullptr;
    }

    auto order = it->second;
    order->is_cancelled = true;
    remove_order_from_book(order);
    orders_lookup_.erase(it);
    return order;
}

bool OrderBook::cancel_order(const std::string& order_id, std::vector<ExecReport>& reports) {
    auto order = erase_order(order_id);
    if (!order) {
        return false;
    }
    report(reports, ExecType::CANCELLED, *order, ExecReason::USER);
    return true;
}

bool OrderBook::modify_order(const std::string& order_id, Quantity new_qty, Price new_price,
                             std::vector<Trade>& generated_trades, std::vector<ExecReport>& reports) {
    auto it = orders_lookup_.find(order_id);
    if (it == orders_lookup_.end()) {
        return false;
    }

    auto order = it->second;

    // Modifying to zero quantity cancels the order
    if (new_qty <= 0) {
        return cancel_order(order_id, reports);
    }

    // The new price and open quantity must follow the same rules as a new order
    Order proposed = *order;
    proposed.price = new_price;
    proposed.quantity = new_qty;
    if (auto error = validate_order(proposed, spec())) {
        proposed.quantity = order->filled_quantity + new_qty;
        proposed.remaining_quantity = new_qty;
        report(reports, ExecType::REJECTED, proposed, error->reason).text = error->text;
        return false;
    }
    if (order->order_type == OrderType::LIMIT_MAKER && would_cross(proposed)) {
        proposed.quantity = order->filled_quantity + new_qty;
        proposed.remaining_quantity = new_qty;
        report(reports, ExecType::REJECTED, proposed, ExecReason::POST_ONLY_WOULD_TAKE).text = post_only_text(proposed);
        return false;
    }

    // Standard exchange rules (new_qty is the new open quantity):
    // 1. If price is unchanged and new qty is less than current remaining quantity, we keep priority.
    if (order->price == new_price && new_qty < order->remaining_quantity) {
        Quantity diff = order->remaining_quantity - new_qty;
        order->remaining_quantity = new_qty;
        order->quantity = order->filled_quantity + new_qty;

        if (order->side == Side::BUY) {
            auto price_it = bids_.find(order->price);
            if (price_it != bids_.end()) {
                price_it->second.total_quantity -= diff;
            }
        } else {
            auto price_it = asks_.find(order->price);
            if (price_it != asks_.end()) {
                price_it->second.total_quantity -= diff;
            }
        }
        report(reports, ExecType::AMENDED, *order);
        return true;
    }

    // 2. Otherwise (price change or qty increase), the order is re-submitted and loses priority.
    // It keeps its identity, owner, policy and fills, so the client sees one AMENDED order.
    erase_order(order_id);

    auto new_order = std::make_shared<Order>();
    new_order->order_id = order_id;
    new_order->symbol = order->symbol;
    new_order->order_type = order->order_type;
    new_order->side = order->side;
    new_order->price = new_price;
    new_order->filled_quantity = order->filled_quantity;
    new_order->fees_paid = order->fees_paid;
    new_order->quantity = order->filled_quantity + new_qty;
    new_order->timestamp = clock_.now_ns();
    new_order->owner_id = order->owner_id;
    new_order->stp_policy = order->stp_policy;
    new_order->market_data_liquidity = order->market_data_liquidity;

    generated_trades = submit_internal(new_order, reports, ExecType::AMENDED);
    return true;
}

Bbo OrderBook::bbo() const {
    Bbo b;
    b.timestamp = clock_.now_ns();
    if (!bids_.empty()) {
        b.bid_price = bids_.begin()->first;
        b.bid_qty = bids_.begin()->second.total_quantity;
    }
    if (!asks_.empty()) {
        b.ask_price = asks_.begin()->first;
        b.ask_qty = asks_.begin()->second.total_quantity;
    }
    return b;
}

Depth OrderBook::depth(int levels) const {
    Depth d;
    d.timestamp = clock_.now_ns();
    auto copy_side = [levels](const auto& book_side, std::vector<DepthLevel>& out) {
        out.reserve(static_cast<size_t>(std::max(0, std::min<int>(levels, static_cast<int>(book_side.size())))));
        for (const auto& [price, level] : book_side) {
            if (static_cast<int>(out.size()) >= levels) {
                break;
            }
            out.push_back(DepthLevel{price, level.total_quantity});
        }
    };
    copy_side(bids_, d.bids);
    copy_side(asks_, d.asks);
    return d;
}

namespace {

// True if an order at this price may trade with a resting level at level_price.
bool crosses(const Order& order, Price level_price) {
    return order.side == Side::BUY ? order.price >= level_price : order.price <= level_price;
}

// Both orders belong to the same participant and the incoming order's policy restricts self-trades
bool is_self_trade(const Order& taker, const Order& maker) {
    return taker.stp_policy != StpPolicy::NONE && !taker.owner_id.empty() && taker.owner_id == maker.owner_id;
}

} // namespace

template <typename BookSide>
void OrderBook::sweep(BookSide& book_side, const std::shared_ptr<Order>& order, bool enforce_limit,
                      std::vector<Trade>& trades, bool& taker_cancelled, std::vector<ExecReport>& reports) {
    auto level_it = book_side.begin();
    while (level_it != book_side.end() && order->remaining_quantity > 0 && !taker_cancelled) {
        if (enforce_limit && !crosses(*order, level_it->first)) {
            break;
        }

        PriceLevel& level = level_it->second;
        auto order_it = level.orders.begin();
        while (order_it != level.orders.end() && order->remaining_quantity > 0) {
            auto maker_order = *order_it;
            if (is_self_trade(*order, *maker_order)) {
                StpPolicy policy = order->stp_policy;
                if (policy == StpPolicy::SKIP) {
                    ++order_it;
                    continue;
                }
                if (policy == StpPolicy::CANCEL_MAKER || policy == StpPolicy::CANCEL_BOTH) {
                    // Remove the own resting order in place (the level itself is handled below)
                    level.total_quantity -= maker_order->remaining_quantity;
                    maker_order->is_cancelled = true;
                    maker_order->has_iterator = false;
                    orders_lookup_.erase(maker_order->order_id);
                    order_it = level.orders.erase(order_it);
                    report(reports, ExecType::CANCELLED, *maker_order, ExecReason::STP_MAKER);
                }
                if (policy == StpPolicy::CANCEL_TAKER || policy == StpPolicy::CANCEL_BOTH) {
                    taker_cancelled = true;
                    break;
                }
                continue;
            }

            Quantity match_qty = std::min(order->remaining_quantity, maker_order->remaining_quantity);
            uint64_t trade_id = ++trade_id_seq_;

            trades.push_back(Trade{
                std::to_string(trade_id),
                symbol_,
                maker_order->price,
                match_qty,
                order->side,
                maker_order->order_id,
                order->order_id,
                clock_.now_ns()
            });

            maker_order->remaining_quantity -= match_qty;
            maker_order->filled_quantity += match_qty;
            order->remaining_quantity -= match_qty;
            order->filled_quantity += match_qty;
            level.total_quantity -= match_qty;

            for (const auto& [filled, liquidity] : {std::make_pair(order, Liquidity::TAKER),
                                                    std::make_pair(maker_order, Liquidity::MAKER)}) {
                // Market-data liquidity is not a participant and pays nothing
                int64_t fee = 0;
                if (!filled->market_data_liquidity) {
                    const FeeSchedule& schedule = fees_.schedule_for(filled->owner_id);
                    fee = fee_for(maker_order->price, match_qty,
                                  liquidity == Liquidity::TAKER ? schedule.taker_rate : schedule.maker_rate);
                }
                filled->fees_paid += fee;
                ExecReport& fill = report(reports, ExecType::FILL, *filled);
                fill.fee = fee;
                fill.last_px = maker_order->price;
                fill.last_qty = match_qty;
                fill.liquidity = liquidity;
                fill.trade_id = trades.back().trade_id;
            }

            if (maker_order->remaining_quantity <= 0) {
                orders_lookup_.erase(maker_order->order_id);
                order_it = level.orders.erase(order_it);
            } else {
                ++order_it;
            }
        }

        // A level that still holds orders here either filled the taker or holds only skipped own
        // orders; in the second case matching continues at the next level
        if (level.orders.empty()) {
            level_it = book_side.erase(level_it);
        } else {
            ++level_it;
        }
    }
}

std::vector<Trade> OrderBook::match_against_book(std::shared_ptr<Order> order, bool enforce_limit,
                                                 bool& taker_cancelled, std::vector<ExecReport>& reports) {
    std::vector<Trade> trades;
    if (order->side == Side::BUY) {
        sweep(asks_, order, enforce_limit, trades, taker_cancelled, reports);
    } else {
        sweep(bids_, order, enforce_limit, trades, taker_cancelled, reports);
    }
    return trades;
}

bool OrderBook::would_cross(const Order& order) const {
    if (order.side == Side::BUY) {
        return !asks_.empty() && crosses(order, asks_.begin()->first);
    }
    return !bids_.empty() && crosses(order, bids_.begin()->first);
}

template <typename BookSide>
bool OrderBook::has_fillable_liquidity(const BookSide& book_side, const Order& order) const {
    // Mirrors sweep(): skipped and cancelled own orders provide no liquidity, and a policy that cancels
    // the taker ends the search at the first own order
    Quantity accumulated_qty = 0;
    for (const auto& [price, level] : book_side) {
        if (!crosses(order, price)) {
            break;
        }
        for (const auto& resting_order : level.orders) {
            if (is_self_trade(order, *resting_order)) {
                if (order.stp_policy == StpPolicy::CANCEL_TAKER || order.stp_policy == StpPolicy::CANCEL_BOTH) {
                    return false;
                }
                continue;
            }
            accumulated_qty += resting_order->remaining_quantity;
            if (accumulated_qty >= order.remaining_quantity) {
                return true;
            }
        }
    }
    return false;
}

bool OrderBook::can_fok_be_fully_filled(std::shared_ptr<Order> order) const {
    if (order->side == Side::BUY) {
        return has_fillable_liquidity(asks_, *order);
    }
    return has_fillable_liquidity(bids_, *order);
}

void OrderBook::add_resting_order(std::shared_ptr<Order> order) {
    orders_lookup_[order->order_id] = order;

    if (order->side == Side::BUY) {
        PriceLevel& level = bids_[order->price];
        level.price = order->price;
        level.total_quantity += order->remaining_quantity;
        
        level.orders.push_back(order);
        order->list_it = std::prev(level.orders.end());
        order->has_iterator = true;
    } else {
        PriceLevel& level = asks_[order->price];
        level.price = order->price;
        level.total_quantity += order->remaining_quantity;

        level.orders.push_back(order);
        order->list_it = std::prev(level.orders.end());
        order->has_iterator = true;
    }
}

void OrderBook::remove_order_from_book(std::shared_ptr<Order> order) {
    if (!order->has_iterator) return;

    if (order->side == Side::BUY) {
        auto price_it = bids_.find(order->price);
        if (price_it != bids_.end()) {
            PriceLevel& level = price_it->second;
            level.total_quantity -= order->remaining_quantity;
            level.orders.erase(order->list_it);
            
            if (level.orders.empty()) {
                bids_.erase(price_it);
            }
        }
    } else {
        auto price_it = asks_.find(order->price);
        if (price_it != asks_.end()) {
            PriceLevel& level = price_it->second;
            level.total_quantity -= order->remaining_quantity;
            level.orders.erase(order->list_it);

            if (level.orders.empty()) {
                asks_.erase(price_it);
            }
        }
    }
    order->has_iterator = false;
}

template <typename BookSide>
void OrderBook::check_side(const BookSide& book_side, Side side, std::vector<std::string>& violations,
                           size_t& resting_orders) const {
    const std::string side_name = sideToString(side);
    for (const auto& [price, level] : book_side) {
        const std::string where = side_name + " level " + format_fixed(price);
        if (level.orders.empty()) {
            violations.push_back(where + ": empty level was not erased");
        }
        if (level.price != price) {
            violations.push_back(where + ": PriceLevel.price is " + format_fixed(level.price));
        }

        Quantity sum = 0;
        for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
            const auto& order = *it;
            const std::string who = where + ", order " + order->order_id;
            ++resting_orders;
            sum += order->remaining_quantity;

            if (order->side != side) {
                violations.push_back(who + ": rests on the wrong side");
            }
            if (order->price != price) {
                violations.push_back(who + ": price " + format_fixed(order->price) + " differs from its level");
            }
            if (order->remaining_quantity <= 0) {
                violations.push_back(who + ": non-positive remaining quantity");
            }
            if (order->is_cancelled) {
                violations.push_back(who + ": cancelled order still queued");
            }
            if (!order->has_iterator || order->list_it != it) {
                violations.push_back(who + ": list back-pointer does not point at its queue position");
            }
            auto found = orders_lookup_.find(order->order_id);
            if (found == orders_lookup_.end() || found->second != order) {
                violations.push_back(who + ": missing from the order lookup");
            }
        }

        if (sum != level.total_quantity) {
            violations.push_back(where + ": total_quantity " + format_fixed(level.total_quantity) +
                                 " != sum of orders " + format_fixed(sum));
        }
    }
}

std::vector<std::string> OrderBook::check_invariants() const {
    std::vector<std::string> violations;
    size_t resting_orders = 0;
    check_side(bids_, Side::BUY, violations, resting_orders);
    check_side(asks_, Side::SELL, violations, resting_orders);

    if (is_crossed()) {
        violations.push_back("book is crossed: best bid " + format_fixed(bids_.begin()->first) + " >= best ask " +
                             format_fixed(asks_.begin()->first));
    }
    if (resting_orders != orders_lookup_.size()) {
        violations.push_back("order lookup holds " + std::to_string(orders_lookup_.size()) +
                             " orders but the queues hold " + std::to_string(resting_orders));
    }
    return violations;
}

bool OrderBook::is_crossed() const {
    return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first;
}
