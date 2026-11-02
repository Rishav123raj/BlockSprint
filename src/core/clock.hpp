#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

// Source of time for the engine. Live mode uses WallClock; simulation drives a SimClock from
// historical event timestamps, so results do not depend on when (or how fast) a replay runs.
class IClock {
public:
    virtual ~IClock() = default;

    // Nanoseconds since the Unix epoch
    virtual uint64_t now_ns() const = 0;
};

class WallClock final : public IClock {
public:
    uint64_t now_ns() const override {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
    }
};

// Manually advanced clock for deterministic replay. Time never moves backwards.
// Not thread-safe: a simulation runs single-threaded.
class SimClock final : public IClock {
public:
    explicit SimClock(uint64_t start_ns = 0) : now_ns_(start_ns) {}

    uint64_t now_ns() const override { return now_ns_; }

    void set(uint64_t t_ns) {
        if (t_ns < now_ns_) {
            throw std::invalid_argument("SimClock cannot move backwards: " + std::to_string(t_ns) + " < " +
                                        std::to_string(now_ns_));
        }
        now_ns_ = t_ns;
    }

    void advance(uint64_t dt_ns) { now_ns_ += dt_ns; }

private:
    uint64_t now_ns_;
};
