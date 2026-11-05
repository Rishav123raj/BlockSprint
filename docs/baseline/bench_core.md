# bench_core results

Engine-only throughput from [bench/bench_core.cpp](../../bench/bench_core.cpp): 200,000 operations per workload, best of 3 runs, Release builds, on the development PC (Windows 10). `sim` means no locking and no callbacks; `live` means locking with every callback registered, as the API server runs it.

| Workload | Config | Before P1.11, MSVC | After P1.11, MSVC | Before P1.11, GCC 15 | After P1.11, GCC 15 |
| :--- | :--- | ---: | ---: | ---: | ---: |
| passive | sim | 57,773 | **852,143** | 20,293 | **841,137** |
| passive | live | 11,536 | **656,492** | 7,626 | **650,877** |
| matching | sim | 39,382 | **638,778** | 17,315 | **693,480** |
| matching | live | 12,908 | **498,947** | 7,634 | **561,402** |
| mirror | sim | 45,690 | **711,629** | 15,593 | **646,744** |
| mirror | live | 9,564 | **528,712** | 6,655 | **520,113** |

Operations per second (2026-11-04). Before P1.11, every order built the BBO as JSON twice (each with an ISO timestamp formatted through `ostringstream` and `put_time`) and compared it field by field as strings, and every limit order with a depth callback also built a JSON depth snapshot. That cost 15–100 µs per order.

After P1.11 an order costs about 1.2–2 µs. The plan's Phase 1 target (≥ 3 M ops/s, mixed workload) is **not met yet**. What's left is mostly allocation:
- shared_ptr orders, `std::list` and `std::map` nodes, and `unordered_map` string keys (D15);
- per-call report vectors;
- string fields copied into reports and trades.

Pooling and intrusive lists (D15) are the next step if the simulator needs it. The simulator's own replay target (P3.18, ≥ 5 M events/s) applies to a different path that doesn't create an `Order` per event.

## End-to-end (`scripts/perf_test.py`, WebSocket, 5,000 orders, 100 in flight)

| | v1.0.0 | After P1.11 |
| :--- | ---: | ---: |
| Throughput | 1,923 orders/s | 6,207 orders/s |
| RTT p50 | 55.1 ms | 15.7 ms |

This number is dominated by the Python client and queueing; see [v1.0.0.md](v1.0.0.md).
