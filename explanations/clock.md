# File: src/core/clock.hpp

The engine's source of time (2.0, P1.7). Every timestamp the engine produces comes from an injected clock, never directly from the system. A backtest therefore gives the same output whenever and however fast it runs.

---

## What it Does

| Class | Use |
| :--- | :--- |
| `IClock` | Interface: `uint64_t now_ns() const`, nanoseconds since the Unix epoch |
| `WallClock` | `std::chrono::system_clock`. The live server's clock (the default for `MatchingEngine`) |
| `SimClock` | Manually driven: `set(t)` jumps to a time, `advance(dt)` moves forward. Throws `std::invalid_argument` if `set` would move time backwards. Not thread-safe: a simulation runs single-threaded |

`MatchingEngine` takes the clock as `std::shared_ptr<const IClock>`; the caller keeps its own pointer to drive a `SimClock`:

```cpp
auto clock = std::make_shared<SimClock>(event_time_ns);
MatchingEngine engine(clock, EngineOptions{false});  // no locking in a single-threaded backtest
// for each historical event:
clock->set(event.time_ns);
engine.submit_order(order);
```

## One read per call

Since P1.18 the engine reads the clock **once per call** and stamps everything that call produces (execution reports, trades, BBO and depth snapshots) with that value. A wall-clock read costs about 39 ns on the development PC, and one call used to read it 4–8 times. All events of one call share a timestamp, as on an exchange, where one request gets one transaction time. Under a `SimClock` nothing changes.

`tests/core/engine_test.cpp` and `tests/core/determinism_test.cpp` check that two runs on a `SimClock` are bit-identical, including trade IDs and timestamps.
