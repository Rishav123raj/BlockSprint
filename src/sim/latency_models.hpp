#pragma once
// Latency models (P3.8, architecture §10). Three delays per strategy connection:
//
//   feed   exchange event (ts_local) -> the strategy sees it
//   entry  the strategy sends a request -> the exchange receives it
//   ack    the exchange reports -> the strategy learns of it
//
// Models (config form, times in microseconds):
//   {"model": "constant", "us": 2000}
//   {"model": "normal", "mean_us": 3000, "sd_us": 500}
//   {"model": "lognormal", "median_us": 3000, "sigma": 0.4}         median x e^(sigma N(0,1))
//   {"model": "empirical", "file": "lat.txt", "unit": "us"}          one value per line; unit ns|us|ms
// Every model also takes "min_us" (default 0) and "max_us" (default none): samples are clamped to them.
//
// A LatencyChannel is one direction of one connection: requests on it never overtake each other
// (arrival = max(send + sample, previous arrival)), as on a TCP connection. Each channel draws from its
// own Rng stream, derived from the run's seed and the channel's name, so adding draws in one place
// (a new strategy, a different feed model) leaves every other channel's samples unchanged.
//
// Regime latency (P6.3): a LatencyRegime scales the delays of the channels it applies to by a multiplier
// that depends on the market's recent message rate, so latency rises when the market is busy. The rate
// is data records per second (changed levels and trades; snapshots don't count) over a sliding window;
// the multiplier is piecewise linear through the configured (rate, multiplier) points and flat beyond
// the first and last. The scaling happens after the model's draw, so it never changes a channel's
// random samples; with no points the multiplier is exactly 1.
#include "rng.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sim {

// An independent Rng for a named stream of a run: seed and name (FNV-1a) mixed through SplitMix64
inline Rng stream_rng(uint64_t seed, std::string_view name) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : name) {
        h = (h ^ c) * 0x100000001b3ULL;
    }
    uint64_t state = seed ^ h;
    return Rng(splitmix64(state));
}

class LatencyModel {
public:
    LatencyModel(double min_ns, double max_ns) : min_ns_(min_ns), max_ns_(max_ns) {
        if (!(min_ns >= 0.0) || !(max_ns >= min_ns)) {
            throw std::invalid_argument("latency: need 0 <= min <= max");
        }
    }
    virtual ~LatencyModel() = default;

    // One delay in nanoseconds, clamped to [min, max]
    uint64_t sample_ns(Rng& rng) const {
        const double x = std::clamp(draw_ns(rng), min_ns_, max_ns_);
        return static_cast<uint64_t>(std::floor(x + 0.5));
    }

    virtual std::string describe() const = 0;

protected:
    virtual double draw_ns(Rng& rng) const = 0;

    std::string bounds() const {
        std::string s;
        if (min_ns_ > 0.0) {
            s += ", min " + std::to_string(min_ns_ / 1000.0) + " us";
        }
        if (max_ns_ < std::numeric_limits<double>::max()) {
            s += ", max " + std::to_string(max_ns_ / 1000.0) + " us";
        }
        return s;
    }

private:
    double min_ns_;
    double max_ns_;
};

class ConstantLatency final : public LatencyModel {
public:
    explicit ConstantLatency(double ns, double min_ns = 0.0, double max_ns = std::numeric_limits<double>::max())
        : LatencyModel(min_ns, max_ns), ns_(ns) {
        if (!(ns >= 0.0)) {
            throw std::invalid_argument("constant latency must be >= 0");
        }
    }
    std::string describe() const override { return "constant " + std::to_string(ns_ / 1000.0) + " us" + bounds(); }

protected:
    double draw_ns(Rng&) const override { return ns_; }

private:
    double ns_;
};

class NormalLatency final : public LatencyModel {
public:
    NormalLatency(double mean_ns, double sd_ns, double min_ns = 0.0,
                  double max_ns = std::numeric_limits<double>::max())
        : LatencyModel(min_ns, max_ns), mean_ns_(mean_ns), sd_ns_(sd_ns) {
        if (!(sd_ns >= 0.0)) {
            throw std::invalid_argument("normal latency: sd must be >= 0");
        }
    }
    std::string describe() const override {
        return "normal mean " + std::to_string(mean_ns_ / 1000.0) + " us, sd " + std::to_string(sd_ns_ / 1000.0) +
               " us" + bounds();
    }

protected:
    double draw_ns(Rng& rng) const override { return rng.normal(mean_ns_, sd_ns_); }

private:
    double mean_ns_;
    double sd_ns_;
};

class LogNormalLatency final : public LatencyModel {
public:
    LogNormalLatency(double median_ns, double sigma, double min_ns = 0.0,
                     double max_ns = std::numeric_limits<double>::max())
        : LatencyModel(min_ns, max_ns), median_ns_(median_ns), sigma_(sigma) {
        if (!(median_ns > 0.0) || !(sigma >= 0.0)) {
            throw std::invalid_argument("lognormal latency: median must be > 0 and sigma >= 0");
        }
    }
    std::string describe() const override {
        return "lognormal median " + std::to_string(median_ns_ / 1000.0) + " us, sigma " + std::to_string(sigma_) +
               bounds();
    }

protected:
    double draw_ns(Rng& rng) const override { return median_ns_ * det_exp(sigma_ * rng.normal()); }

private:
    double median_ns_;
    double sigma_;
};

// Inverse CDF of observed values (e.g. recorded ts_local - ts_exch), interpolated between neighbours
class EmpiricalLatency final : public LatencyModel {
public:
    explicit EmpiricalLatency(std::vector<double> values_ns, double min_ns = 0.0,
                              double max_ns = std::numeric_limits<double>::max(), std::string source = "")
        : LatencyModel(min_ns, max_ns), dist_(std::move(values_ns)), source_(std::move(source)) {}
    std::string describe() const override {
        return "empirical " + std::to_string(dist_.size()) + " values" + (source_.empty() ? "" : " from " + source_) +
               ", median " + std::to_string(dist_.quantile(0.5) / 1000.0) + " us" + bounds();
    }
    const EmpiricalDistribution& distribution() const { return dist_; }

protected:
    double draw_ns(Rng& rng) const override { return dist_.sample(rng); }

private:
    EmpiricalDistribution dist_;
    std::string source_;
};

// Reads one value per line (blank lines and lines starting with '#' skipped), scaled to ns by unit
inline std::vector<double> read_latency_file(const std::string& path, const std::string& unit) {
    double scale = 0.0;
    if (unit == "ns") {
        scale = 1.0;
    } else if (unit == "us") {
        scale = 1e3;
    } else if (unit == "ms") {
        scale = 1e6;
    } else {
        throw std::invalid_argument("latency file unit must be ns, us or ms (got '" + unit + "')");
    }
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("can't read latency file " + path);
    }
    std::vector<double> values;
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        size_t used = 0;
        double v = 0.0;
        try {
            v = std::stod(line.substr(first), &used);
        } catch (const std::exception&) {
            used = 0;
        }
        const std::string rest = line.substr(first + used);
        if (used == 0 || rest.find_first_not_of(" \t\r") != std::string::npos || !(v >= 0.0)) {
            throw std::runtime_error(path + ":" + std::to_string(line_no) + ": not a non-negative number");
        }
        values.push_back(v * scale);
    }
    if (values.empty()) {
        throw std::runtime_error("latency file " + path + " has no values");
    }
    return values;
}

// Builds a model from its config object (see the top of this file)
inline std::unique_ptr<LatencyModel> make_latency_model(const nlohmann::json& cfg) {
    if (!cfg.is_object() || !cfg.contains("model") || !cfg["model"].is_string()) {
        throw std::invalid_argument("latency: expected an object with a \"model\" name, got " + cfg.dump());
    }
    const std::string model = cfg["model"].get<std::string>();
    auto us = [&](const char* key) -> double {
        if (!cfg.contains(key) || !cfg[key].is_number()) {
            throw std::invalid_argument("latency model '" + model + "' needs a number \"" + key + "\"");
        }
        return cfg[key].get<double>() * 1000.0;
    };
    auto number = [&](const char* key) -> double {
        if (!cfg.contains(key) || !cfg[key].is_number()) {
            throw std::invalid_argument("latency model '" + model + "' needs a number \"" + key + "\"");
        }
        return cfg[key].get<double>();
    };
    const double min_ns = cfg.contains("min_us") ? us("min_us") : 0.0;
    const double max_ns = cfg.contains("max_us") ? us("max_us") : std::numeric_limits<double>::max();
    if (model == "constant") {
        return std::make_unique<ConstantLatency>(us("us"), min_ns, max_ns);
    }
    if (model == "normal") {
        return std::make_unique<NormalLatency>(us("mean_us"), us("sd_us"), min_ns, max_ns);
    }
    if (model == "lognormal") {
        return std::make_unique<LogNormalLatency>(us("median_us"), number("sigma"), min_ns, max_ns);
    }
    if (model == "empirical") {
        if (!cfg.contains("file") || !cfg["file"].is_string()) {
            throw std::invalid_argument("latency model 'empirical' needs a \"file\"");
        }
        const std::string file = cfg["file"].get<std::string>();
        const std::string unit = cfg.contains("unit") ? cfg["unit"].get<std::string>() : "us";
        return std::make_unique<EmpiricalLatency>(read_latency_file(file, unit), min_ns, max_ns, file);
    }
    throw std::invalid_argument("unknown latency model '" + model + "' (constant, normal, lognormal, empirical)");
}

struct LatencyRegimeConfig {
    uint64_t window_ns = 1000000000ULL;            // 1 s
    std::vector<std::pair<double, double>> curve;  // (records per second, multiplier); empty = off
    bool feed = true;                              // the channels it applies to
    bool entry = true;
    bool ack = true;

    bool enabled() const { return !curve.empty(); }
};

class LatencyRegime {
public:
    explicit LatencyRegime(LatencyRegimeConfig cfg) : cfg_(std::move(cfg)) {
        if (cfg_.window_ns == 0) {
            throw std::invalid_argument("latency regime: window must be > 0");
        }
        for (size_t i = 0; i < cfg_.curve.size(); ++i) {
            const auto& [rate, mult] = cfg_.curve[i];
            if (!(rate >= 0.0) || !(mult > 0.0) || !std::isfinite(rate) || !std::isfinite(mult)) {
                throw std::invalid_argument("latency regime: curve points need rate >= 0 and multiplier > 0");
            }
            if (i > 0 && !(rate > cfg_.curve[i - 1].first)) {
                throw std::invalid_argument("latency regime: curve rates must be strictly increasing");
            }
        }
    }

    // n data records at time t (non-decreasing)
    void record(uint64_t t, uint64_t n) {
        evict(t);
        recent_.emplace_back(t, n);
        in_window_ += n;
    }

    // Records per second over the window ending at t
    double rate(uint64_t t) {
        evict(t);
        return static_cast<double>(in_window_) * 1e9 / static_cast<double>(cfg_.window_ns);
    }

    // The delay multiplier at t
    double multiplier(uint64_t t) {
        const double m = at_rate(rate(t));
        ++samples_;
        sum_ += m;
        max_ = std::max(max_, m);
        return m;
    }

    // The curve at a rate
    double at_rate(double r) const {
        const auto& c = cfg_.curve;
        if (c.empty()) return 1.0;
        if (r <= c.front().first) return c.front().second;
        if (r >= c.back().first) return c.back().second;
        size_t i = 1;
        while (c[i].first < r) ++i;
        const auto& [r0, m0] = c[i - 1];
        const auto& [r1, m1] = c[i];
        return m0 + (m1 - m0) * (r - r0) / (r1 - r0);
    }

    const LatencyRegimeConfig& config() const { return cfg_; }
    uint64_t samples() const { return samples_; }  // delays scaled
    double mean_multiplier() const { return samples_ ? sum_ / static_cast<double>(samples_) : 1.0; }
    double max_multiplier() const { return samples_ ? max_ : 1.0; }

private:
    void evict(uint64_t t) {
        while (!recent_.empty() && recent_.front().first + cfg_.window_ns <= t) {
            in_window_ -= recent_.front().second;
            recent_.pop_front();
        }
    }

    LatencyRegimeConfig cfg_;
    std::deque<std::pair<uint64_t, uint64_t>> recent_;  // (time, records)
    uint64_t in_window_ = 0;
    uint64_t samples_ = 0;
    double sum_ = 0.0;
    double max_ = 0.0;
};

// One direction of one connection: FIFO, never overtaking
class LatencyChannel {
public:
    LatencyChannel(const LatencyModel& model, Rng rng) : model_(&model), rng_(rng) {}

    // When a message sent at send_ns arrives; scale multiplies the sampled delay (regime latency)
    uint64_t arrival(uint64_t send_ns, double scale = 1.0) {
        uint64_t d = model_->sample_ns(rng_);
        if (scale != 1.0) {
            d = static_cast<uint64_t>(std::floor(static_cast<double>(d) * scale + 0.5));
        }
        uint64_t t = send_ns + d;
        if (t < last_) {
            t = last_;  // would overtake the previous message: arrives right behind it
            ++held_back_;
        }
        last_ = t;
        return t;
    }

    uint64_t last_arrival() const { return last_; }
    // Messages whose sampled delay would have overtaken an earlier one
    uint64_t held_back() const { return held_back_; }

private:
    const LatencyModel* model_;
    Rng rng_;
    uint64_t last_ = 0;
    uint64_t held_back_ = 0;
};

} // namespace sim
