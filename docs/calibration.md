# Calibration (P7.6)

Fits the simulator's queue model and latency so that replays of paper sessions agree with them, and measures the
latency presets that can be measured from here. It builds on the paper session protocol and the parity report
([parity_protocol.md](parity_protocol.md)).

## The grid search

```powershell
uv run python -m research.calibration.calibrate data/paper/2026-11-14_quoter_btc_1 data/paper/2026-11-14_quoter_btc_2 data/paper/2026-11-14_quoter_btc_3 `
    --n 1 1.5 2 2.5 3 4 6 --feed-scale 0.25 0.5 0.75 1 1.25 1.5 --order-scale 1 `
    --out data/paper/calibration_quoter_btc.json
```

`research/calibration/calibrate.py` replays every session at every grid point, once per seed (default 1, 2, 3),
into `<session>/calib/<point>/`, and compares each with the paper run using the parity report. A replay of a
15-minute session takes about 2 s. Points already replayed are reused.

| Axis | Meaning |
| :--- | :--- |
| `n` | the `prob_power` exponent |
| `feed_scale` | the session's measured depth delay (mean and spread) times this |
| `order_scale` | the session's measured order legs (half the round trip each) times this |

- **Score:** per session, (fill ratio difference / 15%)² + (1 s markout difference / scale)², averaged over sessions.
  The markout scale is fixed: the mean seed range of the 1 s markout at the baseline point (the sessions' own
  exponent and measured latency). A point's own seed range would let a noisier point score better.
- **Held-out seeds:** with three seeds the grid's best point partly fits the seeds' noise. The `--top` best
  points and the baseline are replayed again with `--holdout-seeds` (default 4, 5, 6), and the chosen point is
  the best of those there.
- **Acceptance** (both parity checks in every session, the markout within each point's own seed range) is
  reported next to the score.

## Result: PassiveQuoter on BTCUSDT (2026-11-14)

Three 15-minute sessions (`prob_power:2` on the paper server), 42 grid points, three seeds, then four points on
held-out seeds:

| n | feed × | Loss (seeds 1-3) | Loss (seeds 4-6) | Fill ratio diff (4-6) | 1 s markout diff, ticks (4-6) | Sessions passing (4-6) |
| ---: | ---: | ---: | ---: | :--- | :--- | ---: |
| **1.5** | **1** | 0.57 | **0.60** | +0.9%, +4.7%, −3.4% | −3.4, +8.0, −2.8 | 2/3 |
| 1.5 | 1.5 | 0.42 | 0.67 | −7.6%, −0.9%, −8.7% | −8.2, −0.9, +2.3 | 2/3 |
| 1.5 | 1.25 | 0.40 | 0.89 | −5.2%, +1.7%, −6.8% | −9.2, +5.7, −1.5 | 2/3 |
| 2 (baseline) | 1 | 0.75 | 1.13 | +0.4%, +4.4%, −5.2% | −11.3, +1.6, −5.7 | 1/3 |

- **Exponent: 1.5.** It is best on both seed sets, and the baseline is worse on both. n = 1 is far off (loss
  29: too many fills) and n ≥ 2.5 gets steadily worse.
- **Latency: as measured.** The grid preferred the feed delay ×1.25 on seeds 1-3, but that didn't hold on seeds
  4-6, so the measured delay stays. The order legs don't matter for this strategy (×3 changes the loss by 0.01):
  at 1.5 ms they are far below the 500 ms quote refresh.
- **Fill ratio** is within ±15% at every point near the best, on both seed sets.
- **1 s markout** passes in 3/3 sessions at some points on seeds 1-3 but 2/3 at best on seeds 4-6. The check
  depends on how far apart three seeds happen to fall (the tolerance ranges from 1 to 15 ticks between seed
  sets), so a single run of it is noisy. The calibrated point leaves differences of 3-8 ticks (at most 0.011 bps).
  **The acceptance is not met robustly**: more seeds per session or longer sessions would tighten the check.
- **Why not 2?** The paper server itself runs `prob_power:2`, so the queue model alone would give n = 2. The
  replay differs from paper in what it shows the strategy (the whole book, against the server's top 10 every
  100 ms) and in how depth batching meets trades on the server. The lower exponent absorbs those differences
  for this strategy; it is a fit, not a measurement of Binance's queue.

The calibrated profile is [configs/sim/calibrated/paper_btcusdt.json](../configs/sim/calibrated/paper_btcusdt.json):
the queue model and latency for replaying a paper session on this machine. Merge it into a run config:

```python
cfg = {**json.load(open("configs/sim/calibrated/paper_btcusdt.json")), "dataset": ..., "strategy": ...}
```

## Latency presets

| Preset | Order legs | Source |
| :--- | :--- | :--- |
| `binance_spot_home_pc` | lognormal, median 87 ms, σ 0.1, min 80 ms | measured: 60 REST round trips (`GET /api/v3/ping`, kept-alive connection) to `api.binance.com` from the development PC on 2026-11-14: p10 170 ms, median 175 ms, p90 189 ms, max 477 ms; each leg is half |
| `binance_spot_tokyo_vps` | lognormal, median 3 ms, σ 0.3, min 1 ms | an estimate: measure it the same way from a VPS in AWS ap-northeast-1 |

The feed delay of both stays at 100 µs, since the recorder's `ts_local` already contains the network delay.
Changing the home PC preset re-pinned one golden scenario (the TWAP seller, `determinism_test.cpp` and
`test_bindings.py`); MSVC and GCC give the same new hash.
