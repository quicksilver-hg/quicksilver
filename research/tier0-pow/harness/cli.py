from __future__ import annotations
import argparse
from pathlib import Path
from harness.sweep import DriverSpec, run_sweep
from harness.aggregate import aggregate
from harness.thresholds import DEFAULTS
from harness.verdict import evaluate_cell, overall_verdict
from harness.report import render_report
from harness.results import FIELDS

# Default sweep. Param labels must match each driver's accepted args.
# NOTE: provisional — Task 12 reconciles this with the real per-EDGEBITS driver
# binaries (EDGEBITS is compile-time, so each EDGEBITS needs its own binary).
def default_specs(hardware_label: str) -> list[DriverSpec]:
    return [
        DriverSpec("cuckoo", ["./drivers/cuckoo/driver"],
                   ["edgebits=29", "edgebits=30", "edgebits=31"],
                   hardware_label, runs=5, verify_loops=200000),
        DriverSpec("equihash", ["./drivers/equihash/driver"],
                   ["n200k9"], hardware_label, runs=5, verify_loops=20000),
    ]

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="results", help="output directory")
    ap.add_argument("--hardware", default="cpu:unknown")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    rows = run_sweep(default_specs(args.hardware))

    # raw data
    with (out / "raw.csv").open("w") as f:
        f.write(",".join(FIELDS) + "\n")
        for r in rows:
            f.write(",".join(str(getattr(r, x)) for x in FIELDS) + "\n")

    cells = aggregate(rows)
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    decision, reasons = overall_verdict(cells, verdicts)
    (out / "report.md").write_text(
        render_report(cells, verdicts, decision, reasons, DEFAULTS))
    print(f"verdict: {decision}; report at {out / 'report.md'}")
    return 0 if decision == "PROCEED" else 1

if __name__ == "__main__":
    raise SystemExit(main())
