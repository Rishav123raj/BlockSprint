#pragma once
#include "clock.hpp"
#include "order_book.hpp"
#include <mutex>
#include <unordered_map>
#include <string>
#include <functional>
#include <memory>

struct EngineOptions {
    // Guard every call with a mutex (live server). A single-threaded simulation turns this off.
    bool thread_safe = true;
};

// Routes orders to per-symbol order books. Each instance is fully independent: its own books,
// its own trade ID sequence (starting at 1) and its own clock, so several engines can run side by
// side, e.g. one per backtest in a parameter sweep.
class MatchingEngine {
public:
    explicit MatchingEngine(std::shared_ptr<const IClock> clock = std::make_shared<WallClock>(),
                            EngineOptions options = {});
    ~MatchingEngine() = default;
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    using BboCallback = std::function<void(const std::string& symbol, const Bbo& bbo)>;
    using DepthCallback = std::function<void(const std::string& symbol, const Depth& depth)>;
    using TradeCallback = std::function<void(const std::string& symbol, const Trade& trade)>;
    using ExecReportCallback = std::function<void(const ExecReport& report)>;

    void register_bbo_callback(BboCallback cb) { bbo_callback_ = std::move(cb); }
    void register_depth_callback(DepthCallback cb) { depth_callback_ = std::move(cb); }
    void register_trade_callback(TradeCallback cb) { trade_callback_ = std::move(cb); }
    // Receives every execution report, in sequence order, before the trade/BBO/depth callbacks
    void register_exec_report_callback(ExecReportCallback cb) { exec_report_callback_ = std::move(cb); }

    // Each mutating call below can also append the execution reports it produced to `reports`.

    // Submits an order and triggers matching. Rejected (REJECTED report, no trades) if an order with
    // the same ID is resting in that symbol's book (use replace_order) or the quantity is zero.
    std::vector<Trade> submit_order(std::shared_ptr<Order> order, std::vector<ExecReport>* reports = nullptr);

    // Cancels the resting order with the same ID (if any) and submits this one in its place, at the
    // back of the queue
    std::vector<Trade> replace_order(std::shared_ptr<Order> order, std::vector<ExecReport>* reports = nullptr);

    // Trading rules for a symbol (tick size, lot size, quantity range, min notional). They apply to
    // orders and modifications from now on, and survive reset(). Symbols without a spec only get the
    // basic checks (positive quantity, positive price for priced orders).
    void set_symbol_spec(const SymbolSpec& spec);
    std::optional<SymbolSpec> symbol_spec(const std::string& symbol);

    // Fees (P1.13): the default schedule applies to every owner without their own. Both survive reset().
    // A new engine charges nothing; the live server uses Binance's regular tier.
    void set_default_fee_schedule(const FeeSchedule& schedule);
    void set_fee_schedule(const std::string& owner, const FeeSchedule& schedule);
    FeeSchedule fee_schedule(const std::string& owner);  // the schedule that applies to the owner
    bool has_own_fee_schedule(const std::string& owner);  // true if the owner has a schedule of its own

    // True once the symbol has a book (an order was submitted for it since the last reset)
    bool has_symbol(const std::string& symbol);

    // Cancels a resting order. Returns false (and reports nothing) if it is not resting.
    bool cancel_order(const std::string& symbol, const std::string& order_id,
                      std::vector<ExecReport>* reports = nullptr);

    // Amends a resting order to a new TOTAL quantity and optionally a new price, as Binance does
    // (OrderBook::amend_order). Returns false if the order is not resting, or (with a REJECTED report)
    // if the new values break the symbol's rules or a post-only order would trade.
    bool amend_order(const std::string& symbol, const std::string& order_id, Quantity new_total,
                     std::optional<Price> new_price, std::vector<Trade>& generated_trades,
                     std::vector<ExecReport>* reports = nullptr);

    // Modifies a resting order's OPEN quantity and/or price (1.0 semantics). Returns false if the order
    // is not resting, or (with a REJECTED report) if the new values break the symbol's rules.
    bool modify_order(const std::string& symbol, const std::string& order_id, Quantity new_qty, Price new_price,
                      std::vector<Trade>& generated_trades, std::vector<ExecReport>* reports = nullptr);

    // Synchronous state queries. They never create a book: an unknown symbol reads as empty.
    Bbo bbo(const std::string& symbol);
    Depth depth(const std::string& symbol, int levels = 10);

    // The resting orders at one price in queue order, or nullptr (OrderBook::find_level). For single-
    // threaded use (the simulator): the level is valid only until the next mutating call.
    const PriceLevel* find_level(const std::string& symbol, Side side, Price price);

    // The same, in the 1.0 JSON format (for the API, tools and tests; not used on the hot path)
    nlohmann::json get_bbo(const std::string& symbol);
    nlohmann::json get_l2_depth(const std::string& symbol, int depth_levels = 10);

    // Clear all order books (reset state). The trade ID sequence continues, so IDs stay unique.
    void reset();

    // Consistency checks for tests and debugging (see OrderBook::check_invariants)
    std::vector<std::string> check_invariants(const std::string& symbol);
    bool is_crossed(const std::string& symbol);

    const IClock& clock() const { return *clock_; }

private:
    OrderBook& get_or_create_book(const std::string& symbol);

    // Starts a call on the book: reads the clock once and builds reports only if someone receives them
    void begin_call(OrderBook& book, const std::vector<ExecReport>* reports) {
        book.begin_call(clock_->now_ns(), reports != nullptr || static_cast<bool>(exec_report_callback_));
    }

    // The per-call report buffer, reused across calls to avoid an allocation per call. Taking it leaves
    // the member empty, so a call made from inside a callback gets a buffer of its own.
    class ReportBuffer {
    public:
        explicit ReportBuffer(std::vector<ExecReport>& home) : home_(home) { reports.swap(home_); }
        ~ReportBuffer() {
            reports.clear();
            home_.swap(reports);
        }
        ReportBuffer(const ReportBuffer&) = delete;
        ReportBuffer& operator=(const ReportBuffer&) = delete;
        std::vector<ExecReport> reports;

    private:
        std::vector<ExecReport>& home_;
    };

    // The symbol's book, or nullptr (read-only queries must not create books: 1.0 defect D18)
    OrderBook* find_book(const std::string& symbol);

    // Sends reports to the exec report callback and moves them into the caller's vector, if any
    void deliver(std::vector<ExecReport>& produced, std::vector<ExecReport>* reports);

    // Trade, BBO and depth callbacks after a submit or replace
    void publish_after_execution(OrderBook& book, const Order& order, const Bbo& old_bbo,
                                 const std::vector<Trade>& trades);

    // Locks the engine mutex when thread_safe is enabled; otherwise returns an unlocked guard
    std::unique_lock<std::recursive_mutex> lock() const;

    std::shared_ptr<const IClock> clock_;
    EngineOptions options_;
    mutable std::recursive_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
    OrderBook* last_book_ = nullptr;  // the most recently used book, checked before hashing the symbol
    std::vector<ExecReport> report_buffer_;
    std::unordered_map<std::string, SymbolSpec> specs_;
    FeeBook fees_;

    // Last trade ID and last execution report sequence number issued by any book of this engine
    uint64_t trade_id_seq_ = 0;
    uint64_t report_seq_ = 0;

    BboCallback bbo_callback_;
    DepthCallback depth_callback_;
    TradeCallback trade_callback_;
    ExecReportCallback exec_report_callback_;
};
