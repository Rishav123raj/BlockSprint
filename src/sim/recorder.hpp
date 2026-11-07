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
#include <string>
#include <vector>

namespace sim {

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
    Price last_mark_ = 0;
    bool finished_ = false;
    std::string output_hash_;
};

// What a configured run reads: the resolved config and its SHA-256, the data file, its manifest and the
// manifest's SHA-256. For finish()'s meta.
nlohmann::json run_inputs(const RunConfig& rc);

} // namespace sim
