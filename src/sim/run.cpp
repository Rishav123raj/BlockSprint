#include "run.hpp"

#include "recorder.hpp"
#include "strategies/registry.hpp"

#include <chrono>

namespace sim {

RunOutcome run_simulation(const RunConfig& rc, nlohmann::json extra) {
    auto strategy = make_strategy(rc.strategy_name, rc.strategy_params);
    Simulator sim(rc.bse_path, rc.sim);
    const uint32_t s = sim.add_strategy(*strategy, rc.session());
    ResultRecorder recorder(sim, rc.output_dir, rc.sample_ns);

    const auto t0 = std::chrono::steady_clock::now();
    sim.run();
    RunOutcome out;
    out.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out.stats = sim.stats();

    // Throughput over every batch read, fast-forwarded or not: the wall time covers both
    const uint64_t batches = out.stats.fast_forward_batches + out.stats.data_batches;
    nlohmann::json meta = run_inputs(rc);
    for (const auto& [key, value] : extra.items()) {
        meta[key] = value;
    }
    if (!meta.contains("run") || !meta["run"].is_object()) {
        meta["run"] = nlohmann::json::object();
    }
    meta["run"]["wall_seconds"] = out.wall_seconds;
    meta["run"]["batches_per_second"] = out.wall_seconds > 0 ? static_cast<double>(batches) / out.wall_seconds : 0.0;

    out.meta = recorder.finish(meta);
    out.output_hash = recorder.output_hash();
    out.session = sim.gateway().stats(s);
    out.account = sim.gateway().exchange_account(s);
    return out;
}

} // namespace sim
