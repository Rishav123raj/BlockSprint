#pragma once
// Strategies by name, for the run config's strategy.name (P3.14). The reference strategies (P3.15) and
// later ones register here.
//
//   null    never trades: a replay with the full loop, for benchmarks and data checks
#include "strategy.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace sim {

// Builds a strategy from its params; throws std::invalid_argument on an unknown name or a bad or unknown
// parameter (naming the key)
std::unique_ptr<Strategy> make_strategy(const std::string& name, const nlohmann::json& params);

std::vector<std::string> strategy_names();

} // namespace sim
