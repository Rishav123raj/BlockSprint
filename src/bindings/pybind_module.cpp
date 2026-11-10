// blocksprint_sim: the simulator as a Python module (Phase 4, docs/dev_setup.md).
//
// P4.1: the run-config path. A config is a Python dict in the run-config format (configs/sim/*.json,
// docs/simulator_architecture.md section 13.1), resolved exactly as `bs_sim run` resolves it, and run with
// run_simulation, the same call the CLI makes. The GIL is released while the simulation runs.
//
//   import blocksprint_sim as bs
//   meta = bs.run({"dataset": "...", "latency_preset": "zero", "strategy": {"name": "passive_quoter"}},
//                 overrides=["window.from=2026-11-05T02:00:00Z"])
//   meta["output_hash"], meta["sessions"][0]["exchange_account"]["pnl"]
#include "config.hpp"
#include "run.hpp"
#include "strategies/registry.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;
using nlohmann::json;

namespace {

// Python objects cross as JSON text: exact for everything a config holds (strings, integers, floats,
// lists, dicts), and the C++ side keeps a single parser
json to_json(const py::handle& obj) {
    if (py::isinstance<py::str>(obj)) {
        return json::parse(obj.cast<std::string>());  // a config given as JSON text
    }
    const auto text = py::module_::import("json").attr("dumps")(obj).cast<std::string>();
    return json::parse(text);
}

py::object to_python(const json& j) { return py::module_::import("json").attr("loads")(j.dump()); }

sim::RunConfig resolve(const py::handle& config, const std::vector<std::string>& overrides) {
    return sim::resolve_run_config(to_json(config), overrides);
}

} // namespace

PYBIND11_MODULE(blocksprint_sim, m) {
    m.doc() = "BlockSprint market simulator: replay recorded Binance data through the matching engine with "
              "latency and queue models (docs/simulator_architecture.md)";
    m.attr("__version__") = BLOCKSPRINT_VERSION;

    // std::invalid_argument (bad config keys, unknown strategy or parameter) arrives as ValueError
    m.def("strategy_names", &sim::strategy_names, "Names accepted by strategy.name (C++ strategies)");
    m.def("latency_presets", &sim::latency_preset_names, "Names accepted by latency_preset");

    m.def(
        "resolve_config",
        [](const py::object& config, const std::vector<std::string>& overrides) {
            return to_python(resolve(config, overrides).resolved);
        },
        py::arg("config"), py::arg("overrides") = std::vector<std::string>{},
        "The fully resolved run config (defaults < latency preset < config < overrides), as bs_sim config "
        "prints it. config: a dict, or JSON text. overrides: 'key.path=value' strings.");

    m.def(
        "run",
        [](const py::object& config, const std::vector<std::string>& overrides) {
            const sim::RunConfig rc = resolve(config, overrides);
            json extra;
            extra["run"] = {{"command", "blocksprint_sim.run"}, {"overrides", overrides}};
            sim::RunOutcome outcome;
            {
                py::gil_scoped_release release;  // no Python is touched while the simulation runs
                outcome = sim::run_simulation(rc, extra);
            }
            return to_python(outcome.meta);
        },
        py::arg("config"), py::arg("overrides") = std::vector<std::string>{},
        "Runs one simulation as `bs_sim run` does and writes the results directory (output.dir). Returns "
        "run_meta.json as a dict (output_hash, sessions with accounts and stats, counts, inputs).");
}
