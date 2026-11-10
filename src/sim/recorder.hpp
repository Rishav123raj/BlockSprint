#pragma once
// ResultRecorder (P3.13, architecture §12): writes one run's outputs to a directory.
//
//   orders.csv           every strategy request when sent: send and arrival time, request, order fields
//   exec_reports.csv     every report for a strategy's order: exchange time and strategy delivery time
//   request_rejects.csv  every refused cancel / amend / replace
//   bbo_samples.csv      every sample interval, per strategy: the data's BBO, the engine book's BBO (with
//                        strategy orders) and the BBO the strategy sees (feed-delayed)
//   positions.csv        every sample interval and at the end, per strategy: the exchange-time account and
//                        mark-to-market PnL at the data's mid
//   run_meta.json        inputs (resolved config, hashes), window, event counts, final accounts, output hashes
//   output_hash          SHA-256 over the five CSV files: identical runs give identical hashes (P3.17)
//
// CSV first: simple and diffable; Python converts to Parquet (Phase 4). Times are ns since the epoch,
// prices and quantities exact decimals. Everything except caller-supplied metadata (wall time) is a pure
// function of the inputs.
#include "config.hpp"
#include "gateway.hpp"
#include "simulator.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim {

// The CSV rows in memory (P4.5), for Python: fixed-width records that numpy views without a copy. Values are
// the CSV's in raw units: times in ns, prices, quantities and money in 1e-8, enums as their C++ codes.
// Strings are interned: `owner` indexes ResultTables::owners, `order` indexes ResultTables::order_ids.
// A field the CSV leaves empty holds 0 (the rule is given per field).
struct OrderRow {
    uint64_t sent_ns, arrive_ns;
    uint32_t owner, order;
    uint8_t request;                  // Request::Type
    uint8_t side, order_type, stp;    // NEW / REPLACE only
    uint8_t has_new_price;            // AMEND with a new price
    uint8_t pad_[3];
    int64_t price;                    // NEW / REPLACE only
    int64_t quantity;                 // NEW / REPLACE / AMEND (the new total)
    int64_t new_price;                // has_new_price only
};

struct ReportRow {
    uint64_t exchange_ns, deliver_ns, seq;
    uint32_t owner, order;
    uint8_t type, reason, side, order_type;  // reason: empty when NONE
    uint8_t liquidity;                       // FILL only
    uint8_t pad_[7];
    int64_t price, quantity;
    int64_t last_px, last_qty;               // FILL only
    int64_t cum_qty, leaves_qty;
    int64_t fee;                             // FILL only
    int64_t cum_fee;
};

struct RejectRow {
    uint64_t exchange_ns, deliver_ns;
    uint32_t owner, order;
    uint8_t request;                 // RequestReject::Request
    uint8_t reason;                  // empty when NONE
    uint8_t pad_[6];
};

struct BboRow {
    uint64_t ts_ns;
    uint32_t owner;                  // kNoOwner: a run without strategies
    uint32_t pad_;
    // A side with price 0 has no level (both columns empty)
    int64_t data_bid, data_bid_qty, data_ask, data_ask_qty;
    int64_t book_bid, book_bid_qty, book_ask, book_ask_qty;
    int64_t seen_bid, seen_bid_qty, seen_ask, seen_ask_qty;
    uint64_t seen_data_ns;           // 0: the strategy has seen no data yet
};

struct PositionRow {
    uint64_t ts_ns;
    uint32_t owner, pad_;
    int64_t position, cash, fees, mark, pnl;
    uint64_t fills, maker_fills, taker_fills, open_orders;
    int64_t base_volume, quote_volume;
};

struct ResultTables {
    static constexpr uint32_t kNoOwner = 0xFFFFFFFFu;

    std::vector<std::string> owners;     // by session
    std::vector<std::string> order_ids;  // interned, in order of first appearance
    std::vector<OrderRow> orders;
    std::vector<ReportRow> reports;
    std::vector<RejectRow> rejects;
    std::vector<std::string> reject_texts;  // one per reject row
    std::vector<BboRow> bbo;
    std::vector<PositionRow> positions;

    uint32_t order_index(const std::string& id);

private:
    std::unordered_map<std::string, uint32_t> order_index_;
};

class ResultRecorder final : public GatewayListener {
public:
    // Creates dir (and its parents) and replaces the run files in it. Becomes the gateway's listener and,
    // when sample_ns > 0, the simulator's sampler. Attach before sim.run().
    ResultRecorder(Simulator& sim, const std::string& dir, uint64_t sample_ns);
    ~ResultRecorder() override;
    ResultRecorder(const ResultRecorder&) = delete;
    ResultRecorder& operator=(const ResultRecorder&) = delete;

    // After sim.run(): the final positions, then closes and hashes the CSVs and writes run_meta.json (with
    // meta's keys added at the top level) and output_hash. Returns the metadata written.
    nlohmann::json finish(const nlohmann::json& meta = nlohmann::json::object());

    const std::string& output_hash() const { return output_hash_; }
    const std::string& dir() const { return dir_; }

    // Also keeps every row in memory (P4.5). Call before sim.run(); the tables are complete after finish().
    std::shared_ptr<ResultTables> keep_tables();

    // GatewayListener
    void on_request(uint32_t session, const Request& r, uint64_t arrive_at) override;
    void on_report(uint32_t session, const ExecReport& r, uint64_t deliver_at) override;
    void on_request_reject(uint32_t session, const RequestReject& r, uint64_t deliver_at) override;

private:
    struct Output {
        const char* name;
        std::ofstream out;
        uint64_t rows = 0;
    };

    void sample(uint64_t t);
    void write_positions(uint64_t t);
    Price mark();  // the data's mid; else the engine book's; else the last one
    std::ofstream& row(Output& o) {
        ++o.rows;
        return o.out;
    }

    Simulator& sim_;
    std::string dir_;
    Output orders_{"orders.csv", {}, 0};
    Output reports_{"exec_reports.csv", {}, 0};
    Output rejects_{"request_rejects.csv", {}, 0};
    Output bbo_{"bbo_samples.csv", {}, 0};
    Output positions_{"positions.csv", {}, 0};
    std::shared_ptr<ResultTables> tables_;
    Price last_mark_ = 0;
    bool finished_ = false;
    std::string output_hash_;
};

// orders.csv's request column: new / cancel / amend / replace
const char* request_type_name(Request::Type t);

// What a configured run reads: the resolved config and its SHA-256, the data file, its manifest and the
// manifest's SHA-256. For finish()'s meta.
nlohmann::json run_inputs(const RunConfig& rc);

} // namespace sim
