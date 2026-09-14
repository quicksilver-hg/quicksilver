from __future__ import annotations
import io
import sys
import subprocess
from dataclasses import dataclass
from harness.results import Result, FIELDS, parse_row
import csv

@dataclass(frozen=True)
class DriverSpec:
    algorithm: str
    command: list[str]          # e.g. ["./drivers/cuckoo/driver"]
    param_labels: list[str]     # one driver invocation per label
    hardware_label: str
    runs: int
    verify_loops: int

def _parse_driver_stdout(text: str) -> list[Result]:
    # Drivers emit headerless data rows; attach the known schema.
    reader = csv.DictReader(io.StringIO(text), fieldnames=list(FIELDS))
    return [parse_row(r) for r in reader if r["algorithm"]]

def run_sweep(specs: list[DriverSpec]) -> list[Result]:
    rows: list[Result] = []
    for spec in specs:
        # Labels become CSV fields emitted by the (unquoting) native drivers, so a
        # comma in a label would corrupt the row. Reject it loudly rather than
        # silently misalign columns.
        if "," in spec.hardware_label:
            raise ValueError(
                f"hardware_label must not contain ',' (CSV delimiter): "
                f"{spec.hardware_label!r}")
        for label in spec.param_labels:
            if "," in label:
                raise ValueError(
                    f"param_label must not contain ',' (CSV delimiter): {label!r}")
            cmd = spec.command + [label, spec.hardware_label,
                                  str(spec.runs), str(spec.verify_loops)]
            try:
                proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
            except subprocess.CalledProcessError as e:
                # Surface the driver's own diagnostic (e.g. a FATAL self-validation
                # message) instead of just "non-zero exit status".
                if e.stderr:
                    print(e.stderr, end="", file=sys.stderr)
                raise
            rows.extend(_parse_driver_stdout(proc.stdout))
    return rows
