#pragma once
// Run config (P3.12, architecture §13.1): one JSON document describes a simulation run.
//
// Layers, later ones win:  built-in defaults < latency preset (named by "latency_preset") < config file
//                          < --set key.path=value overrides
// Objects merge key by key, and an empty object clears (--set strategy.params={}). A latency model or queue
// model object that names its "model" replaces the previous one whole (so a preset's lognormal parameters
// don't leak into a file's constant model).
// Unknown keys are errors, so a typo can't silently fall back to a default.
//
// The resolved document (every layer applied, times normalized to ns-precision UTC) is what a run records
// in run_meta.json (P3.13).
#include "gateway.hpp"
#include "simulator.hpp"

#include "fees.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim {

struct RunConfig {
    nlohmann::json resolved;

    // Data
    std::string bse_path;      // the symbol's .bse file
    std::string symbol;
    std::string bse_sha256;    // from the dataset manifest; empty when the config names a .bse file directly
    std::string manifest_path; // empty when the config names a .bse file directly

    SimulatorOptions sim;

    // The strategy and its connection
    std::string strategy_name;
    nlohmann::json strategy_params;
    std::string owner;
    std::shared_ptr<const LatencyModel> feed;
    std::shared_ptr<const LatencyModel> entry;
    std::shared_ptr<const LatencyModel> ack;
    FeeSchedule fees{};
    GatewayLimits limits{};

    // Outputs
    std::string output_dir;
    uint64_t sample_ns = 0;

    SessionConfig session() const;
};

// Resolves a parsed config file with overrides ("a.b.c=value"; the value is parsed as JSON when it is
// valid JSON, otherwise taken as a string). Opens the dataset to find the symbol's file. Throws
// std::invalid_argument with the offending key on any error.
RunConfig resolve_run_config(const nlohmann::json& file, const std::vector<std::string>& overrides = {});
RunConfig load_run_config(const std::string& path, const std::vector<std::string>& overrides = {});

// Built-in defaults (without latency: a run must choose a preset or models)
nlohmann::json default_run_config();

// Named latency presets: {"feed": ..., "entry": ..., "ack": ...}. "zero" is exact; the Binance ones are
// placeholders until calibration (P7.6).
nlohmann::json latency_preset(const std::string& name);
std::vector<std::string> latency_preset_names();

// Times: ns since the epoch as a number or digit string, "YYYY-MM-DD" (midnight UTC), or
// "YYYY-MM-DDTHH:MM:SS[.fffffffff]Z"
uint64_t parse_time_ns(const std::string& text);
// "YYYY-MM-DDTHH:MM:SS.fffffffffZ"
std::string format_time_ns(uint64_t ns);

} // namespace sim
