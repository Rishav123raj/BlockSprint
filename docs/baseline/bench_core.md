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

## P1.18: allocation and clock work (2026-11-06)

### Method change

From P1.18, `bench_core` prints two rates per row:
- **engine**: the engine calls alone. Orders are built in batches of 1,024 operations outside the timer, then the batch runs, so each order is still in cache, as it is for a caller that builds an order just before submitting it. Building all 200,000 orders up front (about 40 MB) made every order a cache miss.
- **+orders**: the same plus building each order (`make_shared<Order>` with string IDs), which a caller of this API pays. This is closest to the definition used in the table above.

`sim` now uses `SimClock`, as a backtest does, and `live` uses `WallClock`. One wall-clock read costs about 39 ns on this PC.

### Changes

| Change | Why |
| :--- | :--- |
| One clock read per engine call (`OrderBook::begin_call`) instead of one per report, trade and snapshot | 4–8 reads of ~39 ns per call. Every event of one call now carries the same timestamp (unchanged under `SimClock`) |
| No `ExecReport` is built when nobody receives it (no exec-report callback and no `reports` vector). Sequence numbers still advance | Each report copies 4 strings |
| The per-call report vector is reused (`MatchingEngine::ReportBuffer`) | One allocation per call |
| Intrusive FIFO per price level (`Order::queue_prev/queue_next`) instead of `std::list<std::shared_ptr<Order>>` | One list node allocation plus shared_ptr copies (atomic) per resting order. **D15, first half** |
| A per-book `std::pmr::unsynchronized_pool_resource` for the price maps and the order lookup; the lookup keys on a `string_view` into the order's own ID | Level and lookup nodes no longer use the global heap; long client IDs aren't copied |
| Order pointers passed by reference on the hot path; `FeeBook::schedule_for` skips hashing when no owner has a schedule; the last book is cached; trades reserve room for 4 on the first fill; no BBO work without BBO or depth listeners | Small per-call savings |

Tried and dropped: link-time optimization (`/GL` + `/LTCG`), no measurable change.

A duplicate submission is now rejected before the order object is touched. Resubmitting the very object that is resting used to reset its fields; with intrusive links, that would have unlinked it from its own queue. A regression test covers it.

### Results

Operations per second, 200,000 operations per workload. Each `bench_core` row is the best of 3 repeats; the MSVC columns are the best of 3 runs of `bench_core`. The GCC columns run the same new `bench_core` against the engine before and after P1.18 (MSBuild couldn't build the old tree from the scratch directory's long path).

| Workload | Config | MSVC engine | MSVC +orders | GCC 15 engine, before | GCC 15 engine, after |
| :--- | :--- | ---: | ---: | ---: | ---: |
| passive | sim | **2,793,476** | 2,329,919 | 1,450,570 | 2,912,701 |
| passive | live | 1,146,186 | 1,060,058 | 863,535 | 1,343,153 |
| matching | sim | **2,709,241** | 2,029,696 | 1,012,193 | 2,621,947 |
| matching | live | 1,132,599 | 994,923 | 654,467 | 1,190,232 |
| mirror | sim | **3,203,952** | 2,199,083 | 911,999 | 1,809,549 |
| mirror | live | 1,164,651 | 1,002,342 | 598,044 | 974,976 |

The engine got **2.0–2.6x faster in `sim`** and **1.6–1.8x faster in `live`** (GCC, same benchmark code). Measured on a dual-core i5-6300U laptop (2016) with the market-data recorder running. Two identical binaries differ by up to ±12% between runs here, so small changes were judged by alternating old and new binaries.

**The 3 M ops/s target:** met by `mirror` (3.2 M). `passive` (2.8 M) and the mixed `matching` workload (2.7 M, peaks of 2.86 M in alternating runs) are at 90–95%. With order creation included, all three are at 2.0–2.3 M: building one heap order with string IDs costs about 210 ns, two thirds of the 333 ns budget that 3 M ops/s allows. Going further needs an API that doesn't allocate an `Order` with string IDs per call: integer order handles and an order pool owned by the engine. That belongs to the simulator's event path (P3.18), not to this request/response API.

## End-to-end (`scripts/perf_test.py`, WebSocket, 5,000 orders, 100 in flight)

| | v1.0.0 | After P1.11 |
| :--- | ---: | ---: |
| Throughput | 1,923 orders/s | 6,207 orders/s |
| RTT p50 | 55.1 ms | 15.7 ms |

This number is dominated by the Python client and queueing; see [v1.0.0.md](v1.0.0.md).
