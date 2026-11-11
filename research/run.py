"""python -m research.run EXPERIMENT.yaml: run an experiment through the store (P5.3), then write its tearsheets and
comparison report (P5.9, research/analytics/report.py; --no-report skips them).

research/experiments/runner.py describes what happens; research/experiments/schema.py the file format.
"""

from __future__ import annotations

import argparse
import sys

from research.experiments.runner import plan_runs, run_experiment
from research.experiments.schema import Experiment
from research.experiments.store import Store


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m research.run", description="Run an experiment (YAML) through the "
                                 "results store: cached runs are reused, the rest run in parallel.")
    ap.add_argument("experiment", help="the experiment file (YAML)")
    ap.add_argument("--workers", type=int, help="processes (default: CPUs; 1 runs in this process)")
    ap.add_argument("--force", action="store_true", help="run again even what is in the store")
    ap.add_argument("--results", help="the store's root (default: the experiment's `results`, else results/)")
    ap.add_argument("--dry-run", action="store_true", help="list the runs and whether each is cached; run nothing")
    ap.add_argument("--no-report", action="store_true",
                    help="don't write the tearsheets and the comparison report (research.analytics.report)")
    args = ap.parse_args(argv)

    try:
        exp = Experiment.load(args.experiment)
    except Exception as e:
        print(f"{args.experiment}: {e}", file=sys.stderr)
        return 2
    if args.results:
        exp = exp.model_copy(update={"results": args.results})

    if args.dry_run:
        store = Store(exp.results)
        planned = plan_runs(exp)
        for spec, run_id, _ in planned:
            print(f"{'cached' if store.has(run_id) else 'to run':<7} {run_id}  {spec.label()}")
        print(f"{exp.name}: {len(planned)} runs, {sum(store.has(r) for _, r, _ in planned)} cached")
        return 0

    result = run_experiment(exp, workers=args.workers, force=args.force)
    if not args.no_report and len(result.failed) < len(result.runs):
        from research.analytics.report import write_reports

        write_reports(result, exp)
    for rec in result.failed:
        print(f"\nFAILED {rec.run_id} point {rec.point} seed {rec.seed}:\n{rec.error}", file=sys.stderr)
    return 1 if result.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
