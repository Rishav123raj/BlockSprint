"""Grid-search the queue model's exponent and the latency against paper sessions (P7.6, docs/calibration.md).

Every grid point replays every session (``paper_session.replay_session``) with the point's settings, once per
seed, into ``<session>/calib/<point>/``, and the parity report (``research.analytics.parity``) compares it with
the session's paper run. A point is scored on the two acceptance measures, each in units of its tolerance:

* the fill ratio's relative difference over ``FILL_RATIO_TOL`` (15%);
* the 1 s markout's difference in ticks over a fixed scale: the mean seed range of the 1 s markout at the
  baseline point (the session's own queue model and measured latency). A fixed scale, not each point's own seed
  range, so a point does not score better by being noisier.

The score is the mean over sessions of the sum of squares; the acceptance (both checks in every session, with
each point's own seed-range tolerance) is reported next to it. With a few seeds the grid's best point partly fits
the seeds' noise, so the ``--top`` best points are replayed again with ``--holdout-seeds`` (fresh ones) and the
chosen point is the best of those on the held-out seeds.

The grid's axes:

* ``n``: the ``prob_power`` exponent;
* ``feed_scale``: the session's measured depth delay (mean and spread) times this;
* ``order_scale``: the session's measured order legs (half the round trip each) times this.

Usage::

    uv run python -m research.calibration.calibrate data/paper/2026-11-14_quoter_btc_{1,2,3} \\
        --n 1 1.5 2 3 4 --feed-scale 0.5 1 1.5 --order-scale 1 --out data/paper/calibration.json
"""

from __future__ import annotations

import argparse
import itertools
import json
import logging
from pathlib import Path
from typing import Any

import numpy as np

from research.analytics.parity import FILL_RATIO_TOL, session_report
from research.live.paper_session import default_latency, replay_session

log = logging.getLogger("calibrate")


def label(n: float, feed_scale: float, order_scale: float) -> str:
    return f"n{n:g}_f{feed_scale:g}_o{order_scale:g}"


def scaled_latency(meta: dict[str, Any], feed_scale: float, order_scale: float) -> dict[str, Any]:
    """The session's measured latency (paper_session.default_latency) with the feed delay and order legs scaled."""
    lat = default_latency(meta)
    feed = dict(lat["feed"], mean_us=round(lat["feed"]["mean_us"] * feed_scale),
                sd_us=round(lat["feed"]["sd_us"] * feed_scale))
    leg = {"model": "constant", "us": max(1, round(lat["entry"]["us"] * order_scale))}
    return {"feed": feed, "entry": leg, "ack": dict(leg)}


def evaluate(session: Path, n: float, feed_scale: float, order_scale: float, seeds: list[int],
             tag: str = "") -> dict[str, Any]:
    """Replays the session at one point (unless already there) and returns its parity report."""
    out = session / "calib" / (tag + label(n, feed_scale, order_scale))
    done = out / "replay.json"
    if not (done.exists() and [r["seed"] for r in json.loads(done.read_text(encoding="utf-8"))["runs"]] == seeds):
        meta = json.loads((session / "paper" / "session_meta.json").read_text(encoding="utf-8"))
        replay_session(session, seeds, scaled_latency(meta, feed_scale, order_scale), f"prob_power:{n:g}", out)
    return session_report(session, out)


def score(reports: list[dict[str, Any]], markout_scale: float) -> dict[str, Any]:
    """The point's score (lower is better) and its acceptance."""
    fr = [r["checks"]["fill_ratio_rel"]["value"] for r in reports]
    mo = [r["checks"]["markout_1s_diff_ticks"]["value"] for r in reports]
    loss = float(np.mean([(f / FILL_RATIO_TOL) ** 2 + (m / markout_scale) ** 2 for f, m in zip(fr, mo)]))
    passes = [r["checks"]["fill_ratio_rel"]["pass"] and r["checks"]["markout_1s_diff_ticks"]["pass"] for r in reports]
    return {"loss": loss, "fill_ratio_rel": fr, "markout_1s_diff_ticks": mo,
            "markout_tolerance": [r["checks"]["markout_1s_diff_ticks"]["tolerance"] for r in reports],
            "sessions_passed": int(sum(passes)), "accepted": len(reports) >= 3 and all(passes)}


def calibrate(sessions: list[Path], ns: list[float], feed_scales: list[float], order_scales: list[float],
              seeds: list[int], baseline_n: float, holdout_seeds: list[int] | None = None,
              top: int = 4) -> dict[str, Any]:
    base = [evaluate(s, baseline_n, 1.0, 1.0, seeds) for s in sessions]
    ranges = [r["checks"]["markout_1s_diff_ticks"]["seed_range"] for r in base]
    markout_scale = max(1.0, float(np.mean([x for x in ranges if x is not None]))) if any(ranges) else 1.0
    points = []
    for n, fs, os_ in itertools.product(ns, feed_scales, order_scales):
        reports = [evaluate(s, n, fs, os_, seeds) for s in sessions]
        points.append({"n": n, "feed_scale": fs, "order_scale": os_, **score(reports, markout_scale)})
        log.info("%s: loss %.3f, %d/%d sessions pass", label(n, fs, os_), points[-1]["loss"],
                 points[-1]["sessions_passed"], len(sessions))
    points.sort(key=lambda p: p["loss"])
    result = {"sessions": [s.name for s in sessions], "seeds": seeds, "baseline_n": baseline_n,
              "markout_scale_ticks": markout_scale, "points": points, "best": points[0]}
    if holdout_seeds:
        # The top points and the baseline again, on fresh seeds: the choice is made there
        cands = points[:top] + [p for p in points if (p["n"], p["feed_scale"], p["order_scale"]) == (baseline_n, 1, 1)
                                and p not in points[:top]]
        held = []
        for p in cands:
            reports = [evaluate(s, p["n"], p["feed_scale"], p["order_scale"], holdout_seeds, "holdout_")
                       for s in sessions]
            held.append({"n": p["n"], "feed_scale": p["feed_scale"], "order_scale": p["order_scale"],
                         "grid_loss": p["loss"], **score(reports, markout_scale)})
        held.sort(key=lambda p: p["loss"])
        result.update(holdout_seeds=holdout_seeds, holdout=held, best=held[0])
    return result


def fitted_latency(sessions: list[Path], feed_scale: float, order_scale: float) -> dict[str, Any]:
    """The best point's latency as one profile: the sessions' measured delays averaged, then scaled."""
    lats = [scaled_latency(json.loads((s / "paper" / "session_meta.json").read_text(encoding="utf-8")),
                           feed_scale, order_scale) for s in sessions]
    avg = lambda f: round(float(np.mean([f(x) for x in lats])))  # noqa: E731
    leg = {"model": "constant", "us": avg(lambda x: x["entry"]["us"])}
    return {"feed": {"model": "normal", "mean_us": avg(lambda x: x["feed"]["mean_us"]),
                     "sd_us": avg(lambda x: x["feed"]["sd_us"]), "min_us": 0}, "entry": leg, "ack": dict(leg)}


def _table(points: list[dict[str, Any]], sessions: int) -> list[str]:
    lines = ["| n | feed x | order x | loss | fill ratio diff | 1 s markout diff (ticks) | tolerance | sessions passing |",
             "| ---: | ---: | ---: | ---: | :--- | :--- | :--- | ---: |"]
    for p in points:
        lines.append(f"| {p['n']:g} | {p['feed_scale']:g} | {p['order_scale']:g} | {p['loss']:.2f} | "
                     + ", ".join(f"{x:+.1%}" for x in p["fill_ratio_rel"]) + " | "
                     + ", ".join(f"{x:+.1f}" for x in p["markout_1s_diff_ticks"]) + " | "
                     + ", ".join(f"{x:.1f}" for x in p["markout_tolerance"])
                     + f" | {p['sessions_passed']}/{sessions} |")
    return lines


def render(result: dict[str, Any], top: int = 10) -> str:
    lines = [f"Markout scale {result['markout_scale_ticks']:.1f} ticks; seeds {result['seeds']}", ""]
    lines += _table(result["points"][:top], len(result["sessions"]))
    if result.get("holdout"):
        lines += ["", f"Held-out seeds {result['holdout_seeds']}:", ""] + _table(result["holdout"], len(result["sessions"]))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("sessions", nargs="+", help="paper session directories (paper_session run)")
    p.add_argument("--n", type=float, nargs="+", default=[1, 1.5, 2, 3, 4])
    p.add_argument("--feed-scale", type=float, nargs="+", default=[0.5, 1, 1.5])
    p.add_argument("--order-scale", type=float, nargs="+", default=[1])
    p.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3])
    p.add_argument("--baseline-n", type=float, default=2.0, help="the paper sessions' own exponent")
    p.add_argument("--holdout-seeds", type=int, nargs="*", default=[4, 5, 6])
    p.add_argument("--top", type=int, default=4, help="grid points to check on the held-out seeds")
    p.add_argument("--out", help="write the result here (JSON)")
    args = p.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    logging.getLogger("paper_session").setLevel(logging.WARNING)
    sessions = [Path(s) for s in args.sessions]
    result = calibrate(sessions, args.n, args.feed_scale, args.order_scale, args.seeds, args.baseline_n,
                       args.holdout_seeds, args.top)
    best = result["best"]
    result["fitted_latency"] = fitted_latency(sessions, best["feed_scale"], best["order_scale"])
    if args.out:
        Path(args.out).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(render(result))
    print(f"\nBest: prob_power:{best['n']:g}, latency {json.dumps(result['fitted_latency'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
