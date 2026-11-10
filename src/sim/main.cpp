// bs_sim: the simulator's command line (P3.14).
//
//   bs_sim info FILE.bse                    header, records and batches by kind, segments, time range
//   bs_sim reconstruct FILE.bse [options]   replay the book from the data alone; checkpoint dumps for V0
//   bs_sim synth FILE.bse [options]         replay through the synthesizer into the engine (no strategy)
//   bs_sim latency FILE.bse [options]       exchange -> recorder delays, for latency models
//   bs_sim config CONFIG.json [--set k=v]   the resolved run config (defaults, preset, file, overrides)
//   bs_sim run CONFIG.json [options]        a full simulation: config -> simulator + strategy -> results dir
//   bs_sim slice FILE.bse --from T --to T --out OUT.bse   a self-contained piece of a file (test fixtures)
#include "config.hpp"
#include "reconstruct.hpp"
#include "replayer.hpp"
#include "run.hpp"
#include "simulator.hpp"
#include "synthesizer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sim;

namespace {

void usage() {
    std::cout <<
        "Usage:\n"
        "  bs_sim info FILE.bse\n"
        "  bs_sim reconstruct FILE.bse [--top N] [--every K] [--at TIME]... [--out DUMP.csv]\n"
        "      --top N      levels per side in each checkpoint (default 20)\n"
        "      --every K    checkpoint every K book batches (default 0: none)\n"
        "      --at TIME    checkpoint at a time: ns since the epoch or 2026-11-05T12:00:00Z (repeatable)\n"
        "      --out PATH   write checkpoints as CSV (batch,ts_local,valid,side,level,px,qty)\n"
        "  bs_sim synth FILE.bse [--model M] [--compare-every K] [--top N] [--check-every K]\n"
        "      --model M          queue model (default cancel_from_back; pro_rata, prob_power:2, ...)\n"
        "      --compare-every K  compare the engine's top N with the data every K book batches (default 1)\n"
        "      --top N            levels per side compared (default 20)\n"
        "      --check-every K    full consistency check every K book batches (default 0: only at the end)\n"
        "  bs_sim latency FILE.bse [--kind trade|depth] [--window-s S] [--out VALUES.txt]\n"
        "      ts_local - ts_exch percentiles, raw and above the fastest of each S-second window (default 60);\n"
        "      --out writes the latter (us) for an empirical latency model\n"
        "  bs_sim config CONFIG.json [--set KEY.PATH=VALUE]...\n"
        "      print the resolved run config: defaults < latency preset < file < --set overrides\n"
        "  bs_sim run CONFIG.json [--set KEY.PATH=VALUE]... [--out DIR] [--force]\n"
        "      run a simulation and write its results (orders, exec_reports, request_rejects, bbo_samples,\n"
        "      positions CSVs, run_meta.json, output_hash) to output.dir, or DIR\n"
        "      --force      replace the results of an earlier run in that directory\n"
        "  bs_sim slice FILE.bse --from TIME --to TIME --out OUT.bse\n"
        "      the book at --from as one snapshot, then every record until --to: a small file that replays\n"
        "      on its own (test fixtures)\n";
}

int64_t parse_time(const std::string& text) { return static_cast<int64_t>(parse_time_ns(text)); }

std::string iso(int64_t ns) {
    std::time_t secs = static_cast<std::time_t>(ns / 1000000000LL);
    std::tm tm_val{};
#if defined(_WIN32)
    gmtime_s(&tm_val, &secs);
#else
    gmtime_r(&secs, &tm_val);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm_val.tm_year + 1900, tm_val.tm_mon + 1,
                  tm_val.tm_mday, tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec,
                  static_cast<int>((ns / 1000000) % 1000));
    return buf;
}

// ticks/lots -> decimal text using the header's step (1e-8 units) and decimals
std::string decimal(int64_t units, int64_t step, int decimals) {
    const int64_t fixed = units * step;  // 1e-8 units
    const int64_t whole = fixed / 100000000;
    int64_t frac = fixed % 100000000;
    for (int i = 8; i > decimals; --i) {
        frac /= 10;
    }
    char buf[64];
    if (decimals == 0) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(whole));
    } else {
        std::snprintf(buf, sizeof(buf), "%lld.%0*lld", static_cast<long long>(whole), decimals, static_cast<long long>(frac));
    }
    return buf;
}

void print_header(const bse::Header& h, const std::string& path) {
    static const char* hex = "0123456789abcdef";
    std::string sha;
    for (uint8_t b : h.source_manifest_sha256) {
        sha += hex[b >> 4];
        sha += hex[b & 15];
    }
    std::cout << "file            " << path << "\n"
              << "format          bse v" << h.version << ", " << h.record_size << "-byte records\n"
              << "symbol          " << h.symbol << "\n"
              << "tick / lot      " << decimal(1, h.tick_size, 8) << " / " << decimal(1, h.lot_size, 8)
              << "  (price decimals " << int(h.price_decimals) << ", qty decimals " << int(h.qty_decimals) << ")\n"
              << "min notional    " << decimal(1, h.min_notional, 8) << "\n"
              << "records         " << h.record_count << "\n"
              << "time range      " << iso(h.first_ts_local) << " .. " << iso(h.last_ts_local) << "\n"
              << "source manifest " << sha << "\n";
}

int cmd_info(const std::string& path) {
    Replayer rp(path);
    print_header(rp.header(), path);
    uint64_t by_kind[6] = {};
    uint64_t batches[4] = {};
    uint64_t backwards = 0;
    int64_t prev = INT64_MIN;
    for (uint64_t i = 0; i < rp.record_count(); ++i) {
        const auto& r = rp.records()[i];
        if (r.kind < 6) {
            ++by_kind[r.kind];
        }
        if (r.ts_local < prev) {
            ++backwards;
        }
        prev = r.ts_local;
    }
    auto t0 = std::chrono::steady_clock::now();
    Batch b{};
    while (rp.next(b)) {
        ++batches[static_cast<int>(b.kind)];
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "records by kind ";
    for (int k = 0; k < 6; ++k) {
        std::cout << bse::kind_name(static_cast<uint8_t>(k)) << ' ' << by_kind[k] << (k < 5 ? ", " : "\n");
    }
    std::cout << "batches         depth " << batches[0] << ", snapshot " << batches[1] << ", trade " << batches[2]
              << ", gap " << batches[3] << "  (scanned in " << secs << " s)\n"
              << "segments        " << batches[3] + 1 << "\n"
              << "ts_local        " << (backwards ? "DECREASES " + std::to_string(backwards) + " times" : "non-decreasing")
              << "\n";
    return backwards ? 1 : 0;
}

int cmd_reconstruct(const std::string& path, const std::vector<std::string>& args) {
    ReconstructOptions opt;
    std::string out_path;
    for (size_t i = 0; i < args.size(); ++i) {
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(args[i] + " needs a value");
            }
            return args[++i];
        };
        if (args[i] == "--top") {
            opt.top_n = std::stoul(value());
        } else if (args[i] == "--every") {
            opt.every = std::stoull(value());
        } else if (args[i] == "--at") {
            opt.at_ts.push_back(parse_time(value()));
        } else if (args[i] == "--out") {
            out_path = value();
        } else {
            throw std::invalid_argument("unknown option " + args[i]);
        }
    }
    Replayer rp(path);
    const auto& h = rp.header();
    std::ofstream out;
    if (!out_path.empty()) {
        out.open(out_path, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("can't write " + out_path);
        }
    }
    Reconstructor r;
    auto t0 = std::chrono::steady_clock::now();
    ReconstructStats s = reconstruct(rp, r, opt, out_path.empty() ? nullptr : &out);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::cout << h.symbol << ": " << s.records << " records, " << s.book_batches << " book batches ("
              << s.batches[0] << " depth, " << s.batches[1] << " snapshots, " << s.batches[3] << " gaps), "
              << s.batches[2] << " trades, " << s.segments << " segment(s)\n"
              << "replayed in " << secs << " s: " << static_cast<uint64_t>(s.records / (secs > 0 ? secs : 1e-9))
              << " records/s\n"
              << "crossed after a depth event: " << s.crossed_batches << "; ts_local going back: " << s.ts_backwards
              << "\n";
    if (!out_path.empty()) {
        std::cout << s.checkpoints << " checkpoint(s) of top " << opt.top_n << " written to " << out_path << "\n";
    }

    // Final book, top 5
    std::vector<Level> bids, asks;
    r.book().top(0, 5, bids);
    r.book().top(1, 5, asks);
    std::cout << "final book (" << (r.valid() ? "valid" : "invalid: after a gap") << ")\n";
    for (size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
        std::string bid = i < bids.size() ? decimal(bids[i].qty, h.lot_size, h.qty_decimals) + " @ " +
                                                decimal(bids[i].px, h.tick_size, h.price_decimals) : "";
        std::string ask = i < asks.size() ? decimal(asks[i].px, h.tick_size, h.price_decimals) + " x " +
                                                decimal(asks[i].qty, h.lot_size, h.qty_decimals) : "";
        std::printf("  %28s | %-28s\n", bid.c_str(), ask.c_str());
    }
    return (s.crossed_batches || s.ts_backwards) ? 1 : 0;
}


// Replays a file through the synthesizer into a matching engine with no strategy orders, and measures how
// closely the engine's book follows the data: after a depth event it differs only where a trade hasn't been
// confirmed by depth yet (P3.6)
int cmd_synth(const std::string& path, const std::vector<std::string>& args) {
    std::string model_spec = "cancel_from_back";
    uint64_t compare_every = 1;
    uint64_t check_every = 0;
    size_t top_n = 20;
    for (size_t i = 0; i < args.size(); ++i) {
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(args[i] + " needs a value");
            }
            return args[++i];
        };
        if (args[i] == "--model") {
            model_spec = value();
        } else if (args[i] == "--compare-every") {
            compare_every = std::stoull(value());
        } else if (args[i] == "--check-every") {
            check_every = std::stoull(value());
        } else if (args[i] == "--top") {
            top_n = std::stoul(value());
        } else {
            throw std::invalid_argument("unknown option " + args[i]);
        }
    }
    Replayer rp(path);
    const auto& h = rp.header();
    auto clock = std::make_shared<SimClock>(static_cast<uint64_t>(h.first_ts_local > 0 ? h.first_ts_local : 0));
    MatchingEngine engine(clock, EngineOptions{false});
    SymbolSpec spec;
    spec.symbol = h.symbol;
    spec.tick_size = h.tick_size;
    spec.lot_size = h.lot_size;
    spec.min_qty = h.lot_size;
    spec.min_notional = h.min_notional;
    engine.set_symbol_spec(spec);
    auto model = make_queue_model(model_spec);
    Rng rng(1);
    Synthesizer synth(engine, *model, rng, SynthesizerOptions{h.symbol, h.tick_size, h.lot_size});

    uint64_t book_batches = 0, compared = 0, levels_compared = 0, levels_equal = 0, books_equal = 0, checks = 0;
    std::vector<std::string> violations;
    std::vector<Level> data_levels;
    auto t0 = std::chrono::steady_clock::now();
    double compare_secs = 0;
    Batch b{};
    while (rp.next(b)) {
        if (static_cast<uint64_t>(b.ts_local()) > clock->now_ns()) {
            clock->set(static_cast<uint64_t>(b.ts_local()));
        }
        synth.apply(b);
        if (!b.is_book()) {
            continue;
        }
        ++book_batches;
        if (b.kind == BatchKind::DEPTH && synth.synced() && compare_every && book_batches % compare_every == 0) {
            auto c0 = std::chrono::steady_clock::now();
            const Depth depth = engine.depth(h.symbol, static_cast<int>(top_n));
            bool equal = true;
            for (uint8_t side = 0; side < 2; ++side) {
                synth.data().top(side, top_n, data_levels);
                const auto& ours = side == 0 ? depth.bids : depth.asks;
                for (size_t i = 0; i < data_levels.size(); ++i) {
                    ++levels_compared;
                    const bool same = i < ours.size() && ours[i].price == data_levels[i].px * h.tick_size &&
                                      ours[i].quantity == data_levels[i].qty * h.lot_size;
                    levels_equal += same;
                    equal &= same;
                }
                equal &= ours.size() == data_levels.size();
            }
            ++compared;
            books_equal += equal;
            compare_secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        }
        if (check_every && book_batches % check_every == 0) {
            ++checks;
            for (auto& v : synth.check(false)) {
                violations.push_back("batch " + std::to_string(book_batches) + ": " + v);
            }
        }
    }
    ++checks;
    for (auto& v : synth.check(false)) {
        violations.push_back("end: " + v);
    }
    for (auto& v : engine.check_invariants(h.symbol)) {
        violations.push_back("engine: " + v);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - compare_secs;

    const auto& s = synth.stats();
    auto lots = [&](int64_t q) { return decimal(q / h.lot_size, h.lot_size, h.qty_decimals); };
    std::cout << h.symbol << " through the synthesizer (" << model->name() << ", no strategy)\n"
              << "batches         depth " << s.depth_batches << ", snapshot " << s.snapshots << ", trade " << s.trades
              << ", gap " << s.gaps << "  (unsynced: " << s.depth_unsynced << " depth, " << s.trades_unsynced
              << " trades)\n"
              << "replayed in     " << secs << " s: "
              << static_cast<uint64_t>(static_cast<double>(rp.record_count()) / (secs > 0 ? secs : 1e-9))
              << " records/s (comparisons excluded)\n"
              << "quantities      added " << lots(s.added_qty) << ", cancelled (queue model) " << lots(s.cancelled_qty)
              << ", traded " << lots(s.trade_qty) << " (tape took " << lots(s.tape_synthetic_qty)
              << " synthetic; front removals " << lots(s.trade_front_qty) << ")\n";
    const uint64_t synced_trades = s.trades - s.trades_unsynced;
    std::printf("V4              %llu of %llu synced trades took exactly their quantity at their price (%.4f%%);\n"
                "                through better (stale) levels %s, short (not in the book) %s, of %s traded\n",
                static_cast<unsigned long long>(s.trades_exact), static_cast<unsigned long long>(synced_trades),
                synced_trades ? 100.0 * static_cast<double>(s.trades_exact) / static_cast<double>(synced_trades) : 0.0,
                lots(s.tape_through_qty).c_str(), lots(s.tape_short_qty).c_str(), lots(s.trade_qty).c_str());
    std::cout
              << "dropped         " << lots(s.would_cross_qty) << " (the data itself was crossed)\n";
    if (compared) {
        std::printf("after depth     top %zu equal to the data in %.4f%% of %llu books; %.5f%% of %llu levels\n", top_n,
                    100.0 * static_cast<double>(books_equal) / static_cast<double>(compared),
                    static_cast<unsigned long long>(compared),
                    100.0 * static_cast<double>(levels_equal) / static_cast<double>(levels_compared),
                    static_cast<unsigned long long>(levels_compared));
    }
    std::cout << "consistency     " << checks << " check(s), " << violations.size() << " violation(s)\n";
    for (size_t i = 0; i < violations.size() && i < 10; ++i) {
        std::cout << "  " << violations[i] << "\n";
    }
    return violations.empty() ? 0 : 1;
}


// Delays between Binance's timestamp and the recorder's receipt (ts_local - ts_exch) of one record kind (P3.8).
// The raw values include this machine's clock offset to Binance, which drifts by hundreds of ms over a day
// on a PC. So each value is also taken relative to the fastest message of its window (default 60 s): what
// remains is the delay above the best case, the jitter a latency model needs. --out writes those values
// (microseconds, one per line) for an empirical model.
int cmd_latency(const std::string& path, const std::vector<std::string>& args) {
    std::string kind = "trade";
    std::string out_path;
    int64_t window_ns = 60LL * 1000000000LL;
    for (size_t i = 0; i < args.size(); ++i) {
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(args[i] + " needs a value");
            }
            return args[++i];
        };
        if (args[i] == "--kind") {
            kind = value();
        } else if (args[i] == "--window-s") {
            window_ns = std::stoll(value()) * 1000000000LL;
            if (window_ns <= 0) {
                throw std::invalid_argument("--window-s must be positive");
            }
        } else if (args[i] == "--out") {
            out_path = value();
        } else {
            throw std::invalid_argument("unknown option " + args[i]);
        }
    }
    uint8_t wanted = 0;
    if (kind == "trade") {
        wanted = bse::TRADE;
    } else if (kind == "depth") {
        wanted = bse::DELTA;
    } else {
        throw std::invalid_argument("--kind must be trade or depth");
    }
    Replayer rp(path);
    std::vector<double> us;
    std::vector<int64_t> window_of;
    for (uint64_t i = 0; i < rp.record_count(); ++i) {
        const auto& r = rp.records()[i];
        // One value per depth event (its last record), not per level
        if (r.kind != wanted || (wanted == bse::DELTA && !(r.flags & bse::LAST_IN_BATCH))) {
            continue;
        }
        if (r.flags & bse::TS_CLAMPED) {
            continue;  // ts_local was raised to keep time monotonic: not a real receipt time
        }
        us.push_back(static_cast<double>(r.ts_local - r.ts_exch) / 1000.0);
        window_of.push_back(r.ts_local / window_ns);
    }
    if (us.empty()) {
        std::cout << "no " << kind << " records\n";
        return 1;
    }
    // Relative to the fastest message of the window (records are in ts_local order, so windows are runs)
    std::vector<double> above(us.size());
    for (size_t begin = 0; begin < us.size();) {
        size_t end = begin;
        double fastest = us[begin];
        while (end < us.size() && window_of[end] == window_of[begin]) {
            fastest = std::min(fastest, us[end++]);
        }
        for (size_t i = begin; i < end; ++i) {
            above[i] = us[i] - fastest;
        }
        begin = end;
    }
    if (!out_path.empty()) {
        std::ofstream out(out_path, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("can't write " + out_path);
        }
        out << "# " << rp.header().symbol << " " << kind << " ts_local - ts_exch above the fastest of its "
            << window_ns / 1000000000LL << " s window, us\n";
        char buf[32];
        for (double v : above) {
            std::snprintf(buf, sizeof(buf), "%.3f\n", v);
            out << buf;
        }
    }
    std::printf("%s %s: %zu values, ms\n", rp.header().symbol.c_str(), kind.c_str(), us.size());
    for (const std::vector<double>* values : {&us, &above}) {
        std::vector<double> sorted = *values;
        std::sort(sorted.begin(), sorted.end());
        auto q = [&](double p) { return sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))]; };
        std::printf("  %-14s min %9.3f  p10 %9.3f  p50 %9.3f  p90 %9.3f  p99 %9.3f  p99.9 %9.3f  max %9.3f\n",
                    values == &us ? "raw" : "above fastest", sorted.front() / 1000, q(0.10) / 1000, q(0.50) / 1000,
                    q(0.90) / 1000, q(0.99) / 1000, q(0.999) / 1000, sorted.back() / 1000);
    }
    std::printf("  (raw includes this machine's clock offset to Binance; \"above fastest\" is per %lld s window)\n",
                static_cast<long long>(window_ns / 1000000000LL));
    if (!out_path.empty()) {
        std::cout << "written to " << out_path << " (empirical model: {\"model\": \"empirical\", \"file\": \""
                  << out_path << "\", \"unit\": \"us\", \"min_us\": 0})\n";
    }
    return 0;
}

int cmd_config(const std::string& path, const std::vector<std::string>& args) {
    std::vector<std::string> overrides;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--set" && i + 1 < args.size()) {
            overrides.push_back(args[++i]);
        } else {
            throw std::invalid_argument("unknown option " + args[i]);
        }
    }
    const RunConfig rc = load_run_config(path, overrides);
    std::cout << rc.resolved.dump(2) << "\n";
    std::cerr << "data      " << rc.bse_path << (rc.bse_sha256.empty() ? "" : "  sha256 " + rc.bse_sha256) << "\n"
              << "window    " << (rc.sim.from_ns ? format_time_ns(rc.sim.from_ns) : std::string("file start")) << " .. "
              << (rc.sim.to_ns ? format_time_ns(rc.sim.to_ns) : std::string("file end")) << ", warm-up "
              << static_cast<double>(rc.sim.warmup_ns) / 1e9 << " s\n"
              << "strategy  " << rc.strategy_name << " as " << rc.owner << ", queue model " << rc.sim.queue_model
              << ", seed " << rc.sim.seed << "\n";
    return 0;
}

// stdout of a shell command, trailing newlines removed; empty if it can't run
std::string capture(const std::string& command) {
#if defined(_WIN32)
    FILE* pipe = _popen((command + " 2>NUL").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>/dev/null").c_str(), "r");
#endif
    if (!pipe) {
        return "";
    }
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        out += buf;
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

// The commit the binary was run from (the source tree when built with CMake, else the current directory)
// and whether tracked files had uncommitted changes
nlohmann::json git_info() {
#ifdef BLOCKSPRINT_SOURCE_DIR
    const std::string dir = BLOCKSPRINT_SOURCE_DIR;
#else
    const std::string dir = ".";
#endif
    const std::string git = "git -C \"" + dir + "\" ";
    const std::string sha = capture(git + "rev-parse HEAD");
    if (sha.size() != 40) {
        return nlohmann::json{{"sha", "unknown"}};
    }
    return nlohmann::json{{"sha", sha}, {"dirty", !capture(git + "status --porcelain --untracked-files=no").empty()}};
}

int cmd_run(const std::string& path, const std::vector<std::string>& args) {
    std::vector<std::string> overrides;
    bool force = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--set" && i + 1 < args.size()) {
            overrides.push_back(args[++i]);
        } else if (args[i] == "--out" && i + 1 < args.size()) {
            overrides.push_back("output.dir=" + nlohmann::json(args[++i]).dump());
        } else if (args[i] == "--force") {
            force = true;
        } else {
            throw std::invalid_argument("unknown option " + args[i]);
        }
    }
    const RunConfig rc = load_run_config(path, overrides);
    if (!force && std::filesystem::exists(std::filesystem::path(rc.output_dir) / "run_meta.json")) {
        throw std::invalid_argument("results of an earlier run are in " + rc.output_dir +
                                    " (--force to replace them, or --out DIR)");
    }
    std::cerr << "run       " << rc.strategy_name << " on " << rc.symbol << ", "
              << (rc.sim.from_ns ? format_time_ns(rc.sim.from_ns) : std::string("file start")) << " .. "
              << (rc.sim.to_ns ? format_time_ns(rc.sim.to_ns) : std::string("file end")) << "\n";
    nlohmann::json extra;
    extra["git"] = git_info();
    extra["run"] = {{"command", "bs_sim run " + path}, {"overrides", overrides}};
    const RunOutcome r = run_simulation(rc, extra);

    const SimStats& st = r.stats;
    uint64_t events = 0;
    for (uint64_t n : st.events) {
        events += n;
    }
    const double secs = r.wall_seconds;
    const double batches_per_s = r.meta["run"]["batches_per_second"].get<double>();
    const SessionStats& ss = r.session;
    const Account& a = r.account;
    const nlohmann::json& acct = r.meta["sessions"][0]["exchange_account"];
    std::printf("window    %s .. %s (warm-up to %s)\n", format_time_ns(st.start_ns).c_str(),
                format_time_ns(st.end_ns).c_str(), format_time_ns(st.trading_ns).c_str());
    std::printf("data      %llu batches fast-forwarded, %llu in the window (%llu events); %.2f s, %.2f M batches/s\n",
                static_cast<unsigned long long>(st.fast_forward_batches),
                static_cast<unsigned long long>(st.data_batches), static_cast<unsigned long long>(events), secs,
                batches_per_s / 1e6);
    std::printf("orders    %llu sent, %llu cancels, %llu amends, %llu replaces; %llu rate-limited, %llu refused\n",
                static_cast<unsigned long long>(ss.orders_sent), static_cast<unsigned long long>(ss.cancels_sent),
                static_cast<unsigned long long>(ss.amends_sent), static_cast<unsigned long long>(ss.replaces_sent),
                static_cast<unsigned long long>(ss.rate_limited + ss.open_limited),
                static_cast<unsigned long long>(ss.request_rejects));
    std::printf("fills     %llu (%llu maker, %llu taker), volume %s %s\n", static_cast<unsigned long long>(a.fills),
                static_cast<unsigned long long>(a.maker_fills), static_cast<unsigned long long>(a.taker_fills),
                format_fixed(a.base_volume).c_str(), rc.symbol.c_str());
    std::printf("result    position %s, cash %s, fees %s, PnL %s at mark %s\n", acct["position"].get<std::string>().c_str(),
                acct["cash"].get<std::string>().c_str(), acct["fees"].get<std::string>().c_str(),
                acct["pnl"].get<std::string>().c_str(), r.meta["final_mark"].get<std::string>().c_str());
    std::printf("output    %s  (output_hash %s)\n", rc.output_dir.c_str(), r.output_hash.c_str());
    return 0;
}

int cmd_slice(const std::string& path, const std::vector<std::string>& args) {
    int64_t from = -1, to = -1;
    std::string out;
    for (size_t i = 0; i + 1 < args.size(); i += 2) {
        if (args[i] == "--from") {
            from = parse_time(args[i + 1]);
        } else if (args[i] == "--to") {
            to = parse_time(args[i + 1]);
        } else if (args[i] == "--out") {
            out = args[i + 1];
        } else {
            throw std::invalid_argument("unknown option " + args[i]);
        }
    }
    if (args.size() % 2 || from < 0 || to < 0 || out.empty()) {
        throw std::invalid_argument("slice needs --from TIME --to TIME --out OUT.bse");
    }
    Replayer rp(path);
    const std::vector<bse::Record> records = slice(rp, from, to);
    bse::write_file(out, rp.header(), records);
    std::printf("%s: %llu records, %s .. %s, %.1f KB\n", out.c_str(), static_cast<unsigned long long>(records.size()),
                iso(records.front().ts_local).c_str(), iso(records.back().ts_local).c_str(),
                static_cast<double>(bse::kHeaderSize + records.size() * bse::kRecordSize) / 1024.0);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 2 : 0;
    }
    try {
        if (args[0] == "info" && args.size() == 2) {
            return cmd_info(args[1]);
        }
        if (args[0] == "reconstruct" && args.size() >= 2) {
            return cmd_reconstruct(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[0] == "synth" && args.size() >= 2) {
            return cmd_synth(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[0] == "latency" && args.size() >= 2) {
            return cmd_latency(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[0] == "config" && args.size() >= 2) {
            return cmd_config(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[0] == "run" && args.size() >= 2) {
            return cmd_run(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[0] == "slice" && args.size() >= 2) {
            return cmd_slice(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
