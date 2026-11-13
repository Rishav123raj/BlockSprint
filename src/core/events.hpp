#pragma once
#include "models.hpp"
#include <string>

// Execution reports (P1.10): the complete lifecycle of every order, in the order events happened.
//
// Contract, enforced by tests/core/exec_report_test.cpp:
// * An order the engine refuses gets exactly one REJECTED report and nothing else.
// * An accepted order gets ACK first (AMENDED for an order re-submitted by modify), then any number of
//   FILL / AMENDED reports, then exactly one terminal report: a FILL with leaves_qty == 0, or CANCELLED.
//   An order without a terminal report is resting in the book.
// * Every trade produces two FILL reports with the same trade_id: the taker's, then the maker's.
enum class ExecType {
    ACK,        // accepted (and resting, unless reports for its execution follow)
    FILL,       // partial or complete fill; last_px / last_qty / liquidity / trade_id are set
    AMENDED,    // price or quantity changed by modify
    CANCELLED,  // terminal: removed from the book or remainder discarded; see reason
    REJECTED    // terminal: refused before entering the book; see reason
};

enum class ExecReason {
    NONE,
    // CANCELLED
    USER,              // cancel request, or modify to zero quantity
    REPLACED,          // replaced by a new order with the same ID
    IOC_REMAINDER,     // unfilled part of an IOC order
    NO_LIQUIDITY,      // unfilled part of a MARKET order: no more matchable liquidity
    FOK_KILL,          // FOK order that could not be filled completely
    WOULD_CROSS,       // LIMIT remainder that would rest crossed after self-trade prevention skipped orders
    STP_TAKER,         // incoming order cancelled by self-trade prevention (cancel_taker / cancel_both)
    STP_MAKER,         // resting order cancelled by an incoming order of the same owner (cancel_maker / cancel_both)
    // REJECTED
    DUPLICATE_ORDER_ID,  // an order with the same ID is resting
    INVALID_QUANTITY,    // quantity is zero or negative
    INVALID_PRICE,       // LIMIT / IOC / FOK without a positive price (1.0 defect D6)
    PRICE_TICK,          // price is not a multiple of the symbol's tick size
    QUANTITY_LOT,        // quantity is not a multiple of the symbol's lot size
    QUANTITY_RANGE,      // quantity is below the symbol's minimum or above its maximum
    MIN_NOTIONAL,        // price x quantity is below the symbol's minimum notional
    POST_ONLY_WOULD_TAKE, // LIMIT_MAKER order (or amend) that would trade on arrival instead of resting
    // REJECTED by the simulator's gateway (P3.10), before the engine
    RATE_LIMIT,          // too many orders in the window (Binance: orders per 10 s / per day)
    MAX_OPEN_ORDERS,     // the participant already has the maximum number of open orders on the symbol
    // REJECTED by the strategy's own RiskGuard (P6.4), before the latency pipe
    RISK_POSITION,       // the position could exceed max_position if this and the side's open orders filled
    RISK_NOTIONAL,       // the order's notional is above max_order_notional
    RISK_OPEN_ORDERS,    // the strategy has max_open_orders open or pending orders
    RISK_KILLED,         // the drawdown kill switch is on and the order doesn't reduce the position
    // REJECTED by the simulator's gap policy (P6.5): trading is halted until the feed resyncs
    GAP_HALTED
};

enum class Liquidity { NONE, MAKER, TAKER };

struct ExecReport {
    uint64_t seq = 0;        // per-engine sequence number, starting at 1
    uint64_t timestamp = 0;  // engine clock, ns since the Unix epoch
    ExecType type = ExecType::ACK;
    ExecReason reason = ExecReason::NONE;
    std::string text;        // human-readable detail for REJECTED

    std::string order_id;
    std::string owner_id;
    std::string symbol;
    Side side = Side::BUY;
    OrderType order_type = OrderType::LIMIT;
    Price price = 0;          // the order's limit price (0 for MARKET)
    Quantity quantity = 0;    // the order's quantity as last set (by submit or modify)
    Quantity cum_qty = 0;     // filled so far
    Quantity leaves_qty = 0;  // still open in the book; 0 in terminal reports
    int64_t cum_fee = 0;      // fees charged to this order so far, quote units (1e-8; negative = rebates)

    // FILL only
    Price last_px = 0;
    Quantity last_qty = 0;
    Liquidity liquidity = Liquidity::NONE;
    std::string trade_id;
    int64_t fee = 0;          // fee for this fill (maker or taker rate of the order's owner)
};

inline std::string execTypeToString(ExecType type) {
    switch (type) {
        case ExecType::ACK: return "ack";
        case ExecType::FILL: return "fill";
        case ExecType::AMENDED: return "amended";
        case ExecType::CANCELLED: return "cancelled";
        case ExecType::REJECTED: return "rejected";
    }
    return "unknown";
}

inline std::string execReasonToString(ExecReason reason) {
    switch (reason) {
        case ExecReason::NONE: return "";
        case ExecReason::USER: return "user";
        case ExecReason::REPLACED: return "replaced";
        case ExecReason::IOC_REMAINDER: return "ioc_remainder";
        case ExecReason::NO_LIQUIDITY: return "no_liquidity";
        case ExecReason::FOK_KILL: return "fok_kill";
        case ExecReason::WOULD_CROSS: return "would_cross";
        case ExecReason::STP_TAKER: return "stp_taker";
        case ExecReason::STP_MAKER: return "stp_maker";
        case ExecReason::DUPLICATE_ORDER_ID: return "duplicate_order_id";
        case ExecReason::INVALID_QUANTITY: return "invalid_quantity";
        case ExecReason::INVALID_PRICE: return "invalid_price";
        case ExecReason::PRICE_TICK: return "price_tick";
        case ExecReason::QUANTITY_LOT: return "quantity_lot";
        case ExecReason::QUANTITY_RANGE: return "quantity_range";
        case ExecReason::MIN_NOTIONAL: return "min_notional";
        case ExecReason::POST_ONLY_WOULD_TAKE: return "post_only_would_take";
        case ExecReason::RATE_LIMIT: return "rate_limit";
        case ExecReason::MAX_OPEN_ORDERS: return "max_open_orders";
        case ExecReason::RISK_POSITION: return "risk_position";
        case ExecReason::RISK_NOTIONAL: return "risk_notional";
        case ExecReason::RISK_OPEN_ORDERS: return "risk_open_orders";
        case ExecReason::RISK_KILLED: return "risk_killed";
        case ExecReason::GAP_HALTED: return "gap_halted";
    }
    return "unknown";
}

inline std::string liquidityToString(Liquidity liquidity) {
    switch (liquidity) {
        case Liquidity::NONE: return "";
        case Liquidity::MAKER: return "maker";
        case Liquidity::TAKER: return "taker";
    }
    return "unknown";
}

inline bool is_terminal(const ExecReport& r) {
    return r.type == ExecType::CANCELLED || r.type == ExecType::REJECTED ||
           (r.type == ExecType::FILL && r.leaves_qty == 0);
}

// Prices and quantities are exact decimals (format_fixed), unlike the 1.0 trade format
inline void to_json(nlohmann::json& j, const ExecReport& r) {
    j = nlohmann::json{
        {"seq", r.seq},
        {"timestamp", formatIsoTimestamp(r.timestamp)},
        {"type", execTypeToString(r.type)},
        {"order_id", r.order_id},
        {"symbol", r.symbol},
        {"side", sideToString(r.side)},
        {"order_type", orderTypeToString(r.order_type)},
        {"price", format_fixed(r.price)},
        {"quantity", format_fixed(r.quantity)},
        {"cum_qty", format_fixed(r.cum_qty)},
        {"leaves_qty", format_fixed(r.leaves_qty)},
        {"cum_fee", format_fixed(r.cum_fee)},
    };
    if (r.reason != ExecReason::NONE) {
        j["reason"] = execReasonToString(r.reason);
    }
    if (!r.text.empty()) {
        j["text"] = r.text;
    }
    if (!r.owner_id.empty()) {
        j["owner"] = r.owner_id;
    }
    if (r.type == ExecType::FILL) {
        j["last_px"] = format_fixed(r.last_px);
        j["last_qty"] = format_fixed(r.last_qty);
        j["liquidity"] = liquidityToString(r.liquidity);
        j["trade_id"] = r.trade_id;
        j["fee"] = format_fixed(r.fee);
    }
}
