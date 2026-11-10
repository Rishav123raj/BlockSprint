#pragma once
// One simulation run from a resolved config (P3.17): what `bs_sim run` does, as a library call, so tests
// (the determinism check) run exactly the same path. Builds the strategy from the registry, runs the
// simulator with the result recorder attached, and writes the results directory.
#include "config.hpp"
#include "gateway.hpp"
#include "simulator.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace sim {

struct RunOutcome {
    nlohmann::json meta;      // run_meta.json as written
    std::string output_hash;  // SHA-256 over the CSV outputs (recorder.hpp)
    SimStats stats;
    SessionStats session;     // the strategy's
    Account account;          // the strategy's exchange account at the end
    double wall_seconds = 0;
};

// extra is merged into run_meta.json (after run_inputs); its "run" object gets "wall_seconds" and
// "batches_per_second". Throws std::invalid_argument for a bad strategy name or parameter before any data
// is read.
RunOutcome run_simulation(const RunConfig& rc, nlohmann::json extra = nlohmann::json::object());

} // namespace sim
