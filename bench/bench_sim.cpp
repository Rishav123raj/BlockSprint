// Simulator benchmark (P3.18): records per second through each layer of the simulator, on one .bse file.
//
//   scan          Replayer only: header validation and batch iteration over the memory-mapped file
//   reconstruct   + the L2 book (reconstruction mode, gate V0's path): the plan's "replay-only" figure
//   synth         + the synthesizer into the matching engine (L2 -> L3), no strategy
//   sim null      the full simulator loop (event queue, observed book, gateway) with the null strategy
//   sim quoter    the full loop with PassiveQuoter (0.001, 500 ms, post-only, 10 ms constant latency)
//
// Each stage reads the whole file (or --from/--to for the two sim stages, which fast-forward up to
// --from). Times are wall-clock; the best of --repeat runs is printed.
//
// Usage: bench_sim FILE.bse [--repeat N] [--stages scan,reconstruct,synth,null,quoter]
#include "latency_models.hpp"
#include "queue_models.hpp"
#include "reconstruct.hpp"
#include "replayer.hpp"
#include "rng.hpp"
#include "simulator.hpp"
#include "strategies/passive_quoter.hpp"
#include "synthesizer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sim;

namespace {

struct Result {
    double seconds = 0;
    uint64_t records = 0;
    uint64_t batches = 0;
    std::string note;
};

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Result scan(const std::string& path) {
    Replayer rp(path);
    Result r;
    const double t0 = now_s();
    Batch b{};
    uint64_t sum = 0;
    while (rp.next(b)) {
        ++r.batches;
        sum += b.size();
    }
    r.seconds = now_s() - t0;
    r.records = sum;
    return r;
}

Result reconstruct_stage(const std::string& path) {
    Replayer rp(path);
    Reconstructor rec;
    Result r;
    const double t0 = now_s();
    Batch b{};
    while (rp.next(b)) {
        rec.apply(b);
        ++r.batches;
    }
    r.seconds = now_s() - t0;
    r.records = rp.record_count();
    r.note = std::to_string(rec.book().levels(0) + rec.book().levels(1)) + " levels at the end";
    return r;
}

Result synth_stage(const std::string& path) {
    Replayer rp(path);
    const auto& h = rp.header();
    auto clock = std::make_shared<SimClock>(static_cast<uint64_t>(h.first_ts_local > 0 ? h.first_ts_local : 0));
    MatchingEngine engine(clock, EngineOptions{false, false});
    SymbolSpec spec;
    spec.symbol = h.symbol;
    spec.tick_size = h.tick_size;
    spec.lot_size = h.lot_size;
    spec.min_qty = h.lot_size;
    spec.min_notional = h.min_notional;
    engine.set_symbol_spec(spec);
    auto model = make_queue_model("cancel_from_back");
    Rng rng(1);
    Synthesizer synth(engine, *model, rng, SynthesizerOptions{h.symbol, h.tick_size, h.lot_size});
    Result r;
    const double t0 = now_s();
    Batch b{};
    while (rp.next(b)) {
        if (static_cast<uint64_t>(b.ts_local()) > clock->now_ns()) {
            clock->set(static_cast<uint64_t>(b.ts_local()));
        }
        synth.apply(b);
        ++r.batches;
    }
    r.seconds = now_s() - t0;
    r.records = rp.record_count();
    return r;
}

struct NullStrategy final : Strategy {
    std::string name() const override { return "null"; }
};

Result sim_stage(const std::string& path, bool quoter) {
    SimulatorOptions opt;
    Simulator sim(path, opt);
    NullStrategy null_strategy;
    PassiveQuoter q(PassiveQuoter::Config{100000, 500000000, 1000000, true});
    SessionConfig sc;
    sc.owner = "S1";
    sc.feed = sc.entry = sc.ack = std::make_shared<ConstantLatency>(10e6);
    sc.fees = binance_spot_regular();
    Strategy& s = quoter ? static_cast<Strategy&>(q) : static_cast<Strategy&>(null_strategy);
    sim.add_strategy(s, sc);
    Result r;
    const double t0 = now_s();
    sim.run();
    r.seconds = now_s() - t0;
    const SimStats& st = sim.stats();
    r.batches = st.fast_forward_batches + st.data_batches;
    r.records = Replayer(path).record_count();
    uint64_t events = 0;
    for (uint64_t n : st.events) events += n;
    r.note = std::to_string(events) + " events";
    if (quoter) {
        const Account& a = sim.gateway().exchange_account(0);
        r.note += ", " + std::to_string(sim.gateway().stats(0).orders_sent) + " orders, " + std::to_string(a.fills) +
                  " fills";
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_sim FILE.bse [--repeat N] [--stages scan,reconstruct,synth,null,quoter]\n");
        return 2;
    }
    const std::string path = argv[1];
    int repeat = 1;
    std::string stages = "scan,reconstruct,synth,null,quoter";
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--repeat") {
            repeat = std::max(1, std::atoi(argv[i + 1]));
        } else if (a == "--stages") {
            stages = argv[i + 1];
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    try {
        const Replayer info(path);
        const auto& h = info.header();
        const double span_h = static_cast<double>(h.last_ts_local - h.first_ts_local) / 3.6e12;
        std::printf("file      %s\n          %s, %llu records over %.2f h\n\n", path.c_str(), h.symbol.c_str(),
                    static_cast<unsigned long long>(h.record_count), span_h);
        std::printf("%-12s %9s %14s %14s   %s\n", "stage", "seconds", "M records/s", "M batches/s", "notes");

        const std::vector<std::pair<std::string, std::function<Result()>>> all{
            {"scan", [&] { return scan(path); }},
            {"reconstruct", [&] { return reconstruct_stage(path); }},
            {"synth", [&] { return synth_stage(path); }},
            {"null", [&] { return sim_stage(path, false); }},
            {"quoter", [&] { return sim_stage(path, true); }},
        };
        for (const auto& [name, fn] : all) {
            if (("," + stages + ",").find("," + name + ",") == std::string::npos) continue;
            Result best;
            for (int k = 0; k < repeat; ++k) {
                Result r = fn();
                if (k == 0 || r.seconds < best.seconds) best = r;
            }
            std::printf("%-12s %9.2f %14.2f %14.2f   %s\n", (name == "null" || name == "quoter" ? "sim " + name : name).c_str(),
                        best.seconds, static_cast<double>(best.records) / best.seconds / 1e6,
                        static_cast<double>(best.batches) / best.seconds / 1e6, best.note.c_str());
            std::fflush(stdout);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
