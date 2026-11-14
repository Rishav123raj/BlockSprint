#include "config.hpp"

#include "replayer.hpp"
#include "types.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace sim {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---- time ----

// Days since 1970-01-01 for a civil date and back (Howard Hinnant's algorithms): portable, no timegm/gmtime
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool all_digits(const std::string& s, size_t from, size_t len) {
    if (from + len > s.size()) return false;
    for (size_t i = from; i < from + len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

int field(const std::string& s, size_t from, size_t len) { return std::stoi(s.substr(from, len)); }

// ---- layering ----

bool is_model_path(const std::string& path) {
    return path == "latency.feed" || path == "latency.entry" || path == "latency.ack" || path == "queue_model";
}

void merge(json& base, const json& patch, const std::string& path) {
    // An empty object clears (e.g. --set strategy.params={}); a model object naming its model replaces the old one
    if (!patch.is_object() || !base.is_object() || patch.empty() || (is_model_path(path) && patch.contains("model"))) {
        base = patch;
        return;
    }
    for (const auto& [key, value] : patch.items()) {
        const std::string sub = path.empty() ? key : path + "." + key;
        if (base.contains(key)) {
            merge(base[key], value, sub);
        } else {
            base[key] = value;
        }
    }
}

json override_patch(const std::string& text) {
    const auto eq = text.find('=');
    if (eq == std::string::npos || eq == 0) {
        throw std::invalid_argument("override '" + text + "': expected key.path=value");
    }
    const std::string path = text.substr(0, eq);
    const std::string raw = text.substr(eq + 1);
    json value = json::parse(raw, nullptr, false);
    if (value.is_discarded() || value.is_null()) {
        value = raw;  // not JSON, or "null" (never a useful setting): a plain string (a time, a name, a path)
    }
    json patch = json::object();
    json* at = &patch;
    size_t start = 0;
    while (true) {
        const auto dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (key.empty()) {
            throw std::invalid_argument("override '" + text + "': empty key in the path");
        }
        if (dot == std::string::npos) {
            (*at)[key] = value;
            break;
        }
        (*at)[key] = json::object();
        at = &(*at)[key];
        start = dot + 1;
    }
    return patch;
}

// ---- schema ----

const std::map<std::string, std::set<std::string>>& schema() {
    static const std::map<std::string, std::set<std::string>> keys{
        {"", {"description", "dataset", "symbol", "window", "seed", "tie_break", "latency_preset", "latency",
              "latency_regime", "queue_model", "impact", "fees", "limits", "risk", "gap_policy", "strategy",
              "output"}},
        {"window", {"from", "to", "warmup_s"}},
        {"latency", {"feed", "entry", "ack"}},
        {"latency.feed", {"model", "us", "mean_us", "sd_us", "median_us", "sigma", "file", "unit", "min_us", "max_us"}},
        {"latency.entry", {"model", "us", "mean_us", "sd_us", "median_us", "sigma", "file", "unit", "min_us", "max_us"}},
        {"latency.ack", {"model", "us", "mean_us", "sd_us", "median_us", "sigma", "file", "unit", "min_us", "max_us"}},
        {"latency_regime", {"window_ms", "curve", "channels"}},
        {"queue_model", {"model", "n"}},
        {"impact", {"half_life_ms", "permanent_coefficient", "daily_volatility", "daily_volume"}},
        {"fees", {"maker_rate", "taker_rate"}},
        {"limits", {"orders_per_10s", "orders_per_day", "open_orders", "request_weight_per_min"}},
        {"risk", {"max_position", "max_order_notional", "max_open_orders", "max_drawdown"}},
        {"strategy", {"name", "owner", "params"}},
        {"output", {"dir", "sample_ms"}},
    };
    return keys;
}

void check_keys(const json& j, const std::string& path) {
    auto it = schema().find(path);
    if (it == schema().end() || !j.is_object()) {
        return;
    }
    for (const auto& [key, value] : j.items()) {
        const std::string sub = path.empty() ? key : path + "." + key;
        if (!it->second.count(key)) {
            throw std::invalid_argument("config: unknown key '" + sub + "'");
        }
        check_keys(value, sub);
    }
}

// ---- typed reads (errors name the key) ----

[[noreturn]] void bad(const std::string& key, const std::string& what) {
    throw std::invalid_argument("config: " + key + ": " + what);
}

const json& at(const json& j, const std::string& path) {
    const json* cur = &j;
    size_t start = 0;
    while (true) {
        const auto dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!cur->is_object() || !cur->contains(key)) {
            bad(path, "missing");
        }
        cur = &(*cur)[key];
        if (dot == std::string::npos) {
            return *cur;
        }
        start = dot + 1;
    }
}

std::string get_string(const json& j, const std::string& path) {
    const json& v = at(j, path);
    if (!v.is_string()) bad(path, "expected a string, got " + v.dump());
    return v.get<std::string>();
}

uint64_t get_u64(const json& j, const std::string& path) {
    const json& v = at(j, path);
    if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<int64_t>() >= 0)) {
        bad(path, "expected a non-negative integer, got " + v.dump());
    }
    return v.get<uint64_t>();
}

uint32_t get_u32(const json& j, const std::string& path) {
    const uint64_t v = get_u64(j, path);
    if (v > 0xFFFFFFFFULL) bad(path, "too large");
    return static_cast<uint32_t>(v);
}

double get_nonneg(const json& j, const std::string& path) {
    const json& v = at(j, path);
    if (!v.is_number() || !(v.get<double>() >= 0.0)) bad(path, "expected a non-negative number, got " + v.dump());
    return v.get<double>();
}

uint64_t get_time(const json& j, const std::string& path) {
    const json& v = at(j, path);
    if (v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0)) {
        return v.get<uint64_t>();
    }
    if (!v.is_string()) bad(path, "expected a time (ns, \"YYYY-MM-DD\" or \"YYYY-MM-DDTHH:MM:SSZ\"), got " + v.dump());
    try {
        return parse_time_ns(v.get<std::string>());
    } catch (const std::exception& e) {
        bad(path, e.what());
    }
}

// latency_regime: {"window_ms": 1000, "curve": [[records per s, multiplier], ...], "channels": [...]}
LatencyRegimeConfig latency_regime(const json& r) {
    LatencyRegimeConfig c;
    const double window_ms = get_nonneg(r, "latency_regime.window_ms");
    c.window_ns = static_cast<uint64_t>(std::llround(window_ms * 1e6));
    if (c.window_ns == 0) bad("latency_regime.window_ms", "must be > 0");
    const json curve = at(r, "latency_regime.curve");
    if (!curve.is_array()) bad("latency_regime.curve", "expected [[rate, multiplier], ...], got " + curve.dump());
    for (const auto& p : curve) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) {
            bad("latency_regime.curve", "expected [rate, multiplier] pairs, got " + p.dump());
        }
        c.curve.emplace_back(p[0].get<double>(), p[1].get<double>());
    }
    const json channels = at(r, "latency_regime.channels");
    if (!channels.is_array()) bad("latency_regime.channels", "expected a list of feed / entry / ack");
    c.feed = c.entry = c.ack = false;
    for (const auto& ch : channels) {
        const std::string name = ch.is_string() ? ch.get<std::string>() : ch.dump();
        if (name == "feed") {
            c.feed = true;
        } else if (name == "entry") {
            c.entry = true;
        } else if (name == "ack") {
            c.ack = true;
        } else {
            bad("latency_regime.channels", "'" + name + "' (feed, entry, ack)");
        }
    }
    try {
        LatencyRegime check(c);
    } catch (const std::invalid_argument& e) {
        bad("latency_regime.curve", e.what());
    }
    return c;
}

// A non-negative amount (base or quote units): decimal string (exact) or number
int64_t get_amount(const json& j, const std::string& path) {
    const json& v = at(j, path);
    std::string text;
    if (v.is_string()) {
        text = v.get<std::string>();
    } else if (v.is_number()) {
        text = v.dump();
    } else {
        bad(path, "expected a decimal amount such as \"0.01\", got " + v.dump());
    }
    const auto parsed = parse_fixed(text);
    if (!parsed || *parsed < 0) bad(path, "not a non-negative decimal with at most 8 places: " + text);
    return *parsed;
}

// A fee rate: decimal string (exact) or number, may be negative (rebate)
int64_t get_rate(const json& j, const std::string& path) {
    const json& v = at(j, path);
    std::string text;
    if (v.is_string()) {
        text = v.get<std::string>();
    } else if (v.is_number()) {
        text = v.dump();
    } else {
        bad(path, "expected a decimal rate such as \"0.001\", got " + v.dump());
    }
    const bool negative = !text.empty() && text[0] == '-';
    const auto parsed = parse_fixed(negative ? std::string_view(text).substr(1) : std::string_view(text));
    if (!parsed) bad(path, "not a decimal with at most 8 places: " + text);
    if (*parsed > kFixedScale) bad(path, "a rate above 1 (100%)");
    return negative ? -*parsed : *parsed;
}

std::shared_ptr<const LatencyModel> latency_model(const json& j, const std::string& path) {
    const json& cfg = at(j, path);
    try {
        return std::shared_ptr<const LatencyModel>(make_latency_model(cfg));
    } catch (const std::exception& e) {
        bad(path, e.what());
    }
}

// ---- dataset ----

// One symbol's file: a .bse file, or the symbol's entry of a dataset directory's manifest (symbol may be
// empty when the manifest has one file)
DataFile resolve_file(const std::string& dataset, const std::string& symbol, const std::string& key) {
    DataFile f;
    const fs::path p(dataset);
    if (fs::is_directory(p)) {
        const fs::path manifest = p / "manifest.json";
        std::ifstream in(manifest);
        if (!in) bad(key, "no manifest.json in " + dataset);
        json m;
        try {
            in >> m;
        } catch (const std::exception& e) {
            bad(key, manifest.string() + ": " + e.what());
        }
        if (!m.contains("files") || !m["files"].is_array()) bad(key, manifest.string() + " has no \"files\" list");
        std::vector<std::string> symbols;
        const json* entry = nullptr;
        for (const auto& e : m["files"]) {
            const std::string s = e.value("symbol", "");
            symbols.push_back(s);
            if (s == symbol || (symbol.empty() && m["files"].size() == 1)) {
                entry = &e;
            }
        }
        if (!entry) {
            std::string list;
            for (const auto& s : symbols) list += (list.empty() ? "" : ", ") + s;
            bad("symbol", symbol.empty() ? "the dataset has several symbols (" + list + "): choose one"
                                         : "'" + symbol + "' is not in the dataset (" + list + ")");
        }
        f.bse_path = (p / entry->value("file", "")).generic_string();
        f.bse_sha256 = entry->value("sha256", "");
        f.manifest_path = manifest.generic_string();
    } else if (fs::is_regular_file(p)) {
        f.bse_path = dataset;
    } else {
        bad(key, "no such file or directory: " + dataset);
    }
    Replayer rp(f.bse_path);  // validates the file's header
    if (!symbol.empty() && symbol != rp.header().symbol) {
        bad("symbol", "'" + symbol + "' but " + f.bse_path + " holds " + rp.header().symbol);
    }
    f.symbol = rp.header().symbol;
    return f;
}

// dataset: a directory with a manifest or a .bse file, or (P6.7) a list of .bse files; symbol: one, or a
// list (P6.7). Several symbols make a multi-symbol run, the first the primary. Updates r's symbol.
void resolve_dataset(json& r, RunConfig& rc) {
    std::vector<std::string> wanted;
    if (r.contains("symbol")) {
        const json& s = r["symbol"];
        if (s.is_array()) {
            if (s.empty()) bad("symbol", "an empty list");
            for (size_t i = 0; i < s.size(); ++i) {
                if (!s[i].is_string() || s[i].get<std::string>().empty()) {
                    bad("symbol", "expected a list of symbol names, got " + s.dump());
                }
                wanted.push_back(s[i].get<std::string>());
            }
        } else if (!get_string(r, "symbol").empty()) {
            wanted.push_back(s.get<std::string>());
        }
    }
    const json d = at(r, "dataset");  // a copy: GCC can't see that at() returns into r
    if (d.is_array()) {
        if (d.empty()) bad("dataset", "an empty list");
        if (!wanted.empty() && wanted.size() != d.size()) {
            bad("symbol", "a list of " + std::to_string(d.size()) + " files needs as many symbols (or none)");
        }
        for (size_t i = 0; i < d.size(); ++i) {
            if (!d[i].is_string()) bad("dataset", "expected a list of .bse files, got " + d.dump());
            if (!fs::is_regular_file(d[i].get<std::string>())) bad("dataset", "not a .bse file: " + d[i].get<std::string>());
            rc.files.push_back(resolve_file(d[i].get<std::string>(), wanted.empty() ? "" : wanted[i], "dataset"));
        }
    } else {
        const std::string dataset = get_string(r, "dataset");
        if (wanted.size() <= 1) {
            rc.files.push_back(resolve_file(dataset, wanted.empty() ? "" : wanted[0], "dataset"));
        } else {
            if (!fs::is_directory(dataset)) bad("symbol", "several symbols need a dataset directory or a list of files");
            for (const auto& w : wanted) rc.files.push_back(resolve_file(dataset, w, "dataset"));
        }
    }
    std::set<std::string> seen;
    json names = json::array();
    for (const DataFile& f : rc.files) {
        if (!seen.insert(f.symbol).second) bad("symbol", "'" + f.symbol + "' twice");
        names.push_back(f.symbol);
    }
    const DataFile& primary = rc.files.front();
    rc.symbol = primary.symbol;
    rc.bse_path = primary.bse_path;
    rc.bse_sha256 = primary.bse_sha256;
    rc.manifest_path = primary.manifest_path;
    r["symbol"] = rc.files.size() == 1 ? json(rc.symbol) : names;
}

} // namespace

uint64_t parse_time_ns(const std::string& text) {
    if (!text.empty() && all_digits(text, 0, text.size())) {
        return std::stoull(text);
    }
    const bool date_only = text.size() == 10;
    if (text.size() < 10 || !all_digits(text, 0, 4) || text[4] != '-' || !all_digits(text, 5, 2) || text[7] != '-' ||
        !all_digits(text, 8, 2) ||
        (!date_only && (text.size() < 20 || text[10] != 'T' || !all_digits(text, 11, 2) || text[13] != ':' ||
                        !all_digits(text, 14, 2) || text[16] != ':' || !all_digits(text, 17, 2) || text.back() != 'Z'))) {
        throw std::invalid_argument("bad time '" + text + "' (use ns, YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS[.fff]Z)");
    }
    const int y = field(text, 0, 4), mo = field(text, 5, 2), d = field(text, 8, 2);
    int h = 0, mi = 0, s = 0;
    uint64_t frac_ns = 0;
    if (!date_only) {
        h = field(text, 11, 2);
        mi = field(text, 14, 2);
        s = field(text, 17, 2);
        if (text.size() > 20) {
            const size_t digits = text.size() - 21;
            if (text[19] != '.' || digits == 0 || digits > 9 || !all_digits(text, 20, digits)) {
                throw std::invalid_argument("bad time '" + text + "': fraction must be 1 to 9 digits");
            }
            std::string frac = text.substr(20, digits);
            frac.resize(9, '0');
            frac_ns = std::stoull(frac);
        }
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) {
        throw std::invalid_argument("bad time '" + text + "': field out of range");
    }
    const int64_t secs = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 +
                         h * 3600 + mi * 60 + s;
    return static_cast<uint64_t>(secs) * 1000000000ULL + frac_ns;
}

std::string format_time_ns(uint64_t ns) {
    const uint64_t secs = ns / 1000000000ULL;
    int64_t y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(static_cast<int64_t>(secs / 86400), y, m, d);
    const uint64_t sod = secs % 86400;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02u:%02u:%02u.%09lluZ", static_cast<long long>(y), m, d,
                  static_cast<unsigned>(sod / 3600), static_cast<unsigned>(sod / 60 % 60),
                  static_cast<unsigned>(sod % 60), static_cast<unsigned long long>(ns % 1000000000ULL));
    return buf;
}

json default_run_config() {
    return json{
        {"window", {{"from", 0}, {"to", 0}, {"warmup_s", 0}}},
        {"seed", 0},
        {"tie_break", "data_first"},
        {"queue_model", {{"model", "cancel_from_back"}}},
        {"impact", {{"half_life_ms", 0}, {"permanent_coefficient", 0}, {"daily_volatility", 0}, {"daily_volume", "0"}}},
        {"latency_regime", {{"window_ms", 1000}, {"curve", json::array()}, {"channels", {"feed", "entry", "ack"}}}},
        {"fees", {{"maker_rate", "0.001"}, {"taker_rate", "0.001"}}},  // Binance spot, regular tier
        {"limits", {{"orders_per_10s", 100}, {"orders_per_day", 200000}, {"open_orders", 200},
                    {"request_weight_per_min", 6000}}},
        {"risk", {{"max_position", "0"}, {"max_order_notional", "0"}, {"max_open_orders", 0}, {"max_drawdown", "0"}}},
        {"gap_policy", "halt"},
        {"strategy", {{"owner", "S1"}, {"params", json::object()}}},
        {"output", {{"dir", "results/run"}, {"sample_ms", 1000}}},
    };
}

json latency_preset(const std::string& name) {
    auto constant = [](double us) { return json{{"model", "constant"}, {"us", us}}; };
    auto lognormal = [](double median_us, double sigma, double min_us) {
        return json{{"model", "lognormal"}, {"median_us", median_us}, {"sigma", sigma}, {"min_us", min_us}};
    };
    if (name == "zero") {
        return json{{"feed", constant(0)}, {"entry", constant(0)}, {"ack", constant(0)}};
    }
    // Strategy and recorder on an AWS Tokyo VPS (ap-northeast-1), next to Binance's matching engine
    if (name == "binance_spot_tokyo_vps") {
        return json{{"feed", constant(100)}, {"entry", lognormal(3000, 0.3, 1000)}, {"ack", lognormal(3000, 0.3, 1000)}};
    }
    // Strategy and recorder on the development PC. ts_local already includes the PC's network delay, so the
    // feed adds only processing time; orders cross the same distance one way each: half the REST round trip
    // to api.binance.com measured from that PC (P7.6, docs/calibration.md: median 175 ms, p90 189 ms).
    if (name == "binance_spot_home_pc") {
        return json{{"feed", constant(100)}, {"entry", lognormal(87000, 0.1, 80000)},
                    {"ack", lognormal(87000, 0.1, 80000)}};
    }
    std::string names;
    for (const auto& n : latency_preset_names()) names += (names.empty() ? "" : ", ") + n;
    throw std::invalid_argument("config: latency_preset: unknown preset '" + name + "' (" + names + ")");
}

std::vector<std::string> latency_preset_names() { return {"zero", "binance_spot_tokyo_vps", "binance_spot_home_pc"}; }

std::vector<std::string> RunConfig::bse_paths() const {
    std::vector<std::string> paths;
    for (const DataFile& f : files) paths.push_back(f.bse_path);
    if (paths.empty()) paths.push_back(bse_path);
    return paths;
}

SessionConfig RunConfig::session() const {
    SessionConfig c;
    c.owner = owner;
    c.feed = feed;
    c.entry = entry;
    c.ack = ack;
    c.fees = fees;
    c.limits = limits;
    c.risk = risk;
    c.gap = gap;
    return c;
}

RunConfig resolve_run_config(const json& file, const std::vector<std::string>& overrides) {
    if (!file.is_object()) {
        throw std::invalid_argument("config: expected a JSON object");
    }
    // The file with the overrides applied decides the preset; the preset sits below both
    json user = file;
    for (const auto& o : overrides) {
        merge(user, override_patch(o), "");
    }
    json r = default_run_config();
    if (user.contains("latency_preset")) {
        merge(r, json{{"latency", latency_preset(get_string(user, "latency_preset"))}}, "");
    }
    merge(r, user, "");
    if (r["queue_model"].is_string()) {
        r["queue_model"] = json{{"model", r["queue_model"]}};
    }
    check_keys(r, "");

    RunConfig rc;
    resolve_dataset(r, rc);

    SimulatorOptions& o = rc.sim;
    o.from_ns = get_time(r, "window.from");
    o.to_ns = get_time(r, "window.to");
    o.warmup_ns = static_cast<uint64_t>(std::llround(get_nonneg(r, "window.warmup_s") * 1e9));
    if (o.to_ns && o.to_ns <= o.from_ns) bad("window.to", "must be after window.from");
    if (o.to_ns && o.from_ns && o.warmup_ns >= o.to_ns - o.from_ns) bad("window.warmup_s", "covers the whole window");
    r["window"]["from"] = o.from_ns ? json(format_time_ns(o.from_ns)) : json(0);
    r["window"]["to"] = o.to_ns ? json(format_time_ns(o.to_ns)) : json(0);
    o.seed = get_u64(r, "seed");
    const std::string tie = get_string(r, "tie_break");
    if (tie == "data_first") {
        o.tie_break = TieBreak::DATA_FIRST;
    } else if (tie == "orders_first") {
        o.tie_break = TieBreak::ORDERS_FIRST;
    } else {
        bad("tie_break", "'" + tie + "' (data_first or orders_first)");
    }
    o.queue_model = get_string(r, "queue_model.model");
    if (r["queue_model"].contains("n")) {
        o.queue_model += ":" + at(r, "queue_model.n").dump();
    }
    try {
        make_queue_model(o.queue_model);
    } catch (const std::invalid_argument& e) {
        bad("queue_model", e.what());
    }
    o.impact_half_life_ns = static_cast<uint64_t>(std::llround(get_nonneg(r, "impact.half_life_ms") * 1e6));
    PermanentImpactConfig& pi = o.permanent_impact;
    pi.coefficient = get_nonneg(r, "impact.permanent_coefficient");
    pi.daily_volatility = get_nonneg(r, "impact.daily_volatility");
    pi.daily_volume = get_amount(r, "impact.daily_volume");
    if (pi.enabled() && pi.daily_volatility <= 0) bad("impact.daily_volatility", "must be positive with a permanent_coefficient");
    if (pi.enabled() && pi.daily_volume <= 0) bad("impact.daily_volume", "must be positive with a permanent_coefficient");

    if (!r.contains("latency")) {
        bad("latency", "missing: set latency_preset (" + std::string("zero, binance_spot_tokyo_vps, binance_spot_home_pc") +
                           ") or latency.feed / entry / ack");
    }
    rc.feed = latency_model(r, "latency.feed");
    rc.entry = latency_model(r, "latency.entry");
    rc.ack = latency_model(r, "latency.ack");
    o.latency_regime = latency_regime(r);

    rc.fees = FeeSchedule{get_rate(r, "fees.maker_rate"), get_rate(r, "fees.taker_rate")};
    rc.limits.orders_per_10s = get_u32(r, "limits.orders_per_10s");
    rc.limits.orders_per_day = get_u32(r, "limits.orders_per_day");
    rc.limits.max_open_orders = get_u32(r, "limits.open_orders");
    rc.limits.request_weight_per_min = get_u32(r, "limits.request_weight_per_min");
    rc.risk.max_position = get_amount(r, "risk.max_position");
    rc.risk.max_order_notional = get_amount(r, "risk.max_order_notional");
    rc.risk.max_open_orders = get_u32(r, "risk.max_open_orders");
    rc.risk.max_drawdown = get_amount(r, "risk.max_drawdown");
    try {
        rc.gap = parse_gap_policy(get_string(r, "gap_policy"));
    } catch (const std::invalid_argument& e) {
        bad("gap_policy", e.what());
    }

    rc.strategy_name = get_string(r, "strategy.name");
    if (rc.strategy_name.empty()) bad("strategy.name", "empty");
    rc.owner = get_string(r, "strategy.owner");
    if (rc.owner.empty() || rc.owner == "MARKET" || rc.owner == "MARKET_TAPE") {
        bad("strategy.owner", "'" + rc.owner + "' is reserved or empty");
    }
    rc.strategy_params = at(r, "strategy.params");
    if (!rc.strategy_params.is_object()) bad("strategy.params", "expected an object");

    rc.output_dir = get_string(r, "output.dir");
    rc.sample_ns = static_cast<uint64_t>(std::llround(get_nonneg(r, "output.sample_ms") * 1e6));

    rc.resolved = std::move(r);
    return rc;
}

RunConfig load_run_config(const std::string& path, const std::vector<std::string>& overrides) {
    std::ifstream in(path);
    if (!in) {
        throw std::invalid_argument("config: cannot open " + path);
    }
    json file;
    try {
        in >> file;
    } catch (const std::exception& e) {
        throw std::invalid_argument("config: " + path + ": " + e.what());
    }
    return resolve_run_config(file, overrides);
}

} // namespace sim
