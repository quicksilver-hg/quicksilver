from __future__ import annotations
import csv
from dataclasses import dataclass
from typing import Iterable

FIELDS = (
    "algorithm", "param_label", "hardware_label", "run_index",
    "create_ms", "verify_us", "proof_bytes", "verify_peak_kb", "create_peak_kb",
)

@dataclass(frozen=True)
class Result:
    algorithm: str
    param_label: str
    hardware_label: str
    run_index: int
    create_ms: float
    verify_us: float
    proof_bytes: int
    verify_peak_kb: int
    create_peak_kb: int

def parse_row(row: dict[str, str]) -> Result:
    return Result(
        algorithm=row["algorithm"],
        param_label=row["param_label"],
        hardware_label=row["hardware_label"],
        run_index=int(row["run_index"]),
        create_ms=float(row["create_ms"]),
        verify_us=float(row["verify_us"]),
        proof_bytes=int(row["proof_bytes"]),
        verify_peak_kb=int(row["verify_peak_kb"]),
        create_peak_kb=int(row["create_peak_kb"]),
    )

def load_results(fp: Iterable[str]) -> list[Result]:
    reader = csv.DictReader(fp)
    if reader.fieldnames is None or tuple(reader.fieldnames) != FIELDS:
        raise ValueError(f"unexpected CSV columns: {reader.fieldnames!r}; want {FIELDS!r}")
    return [parse_row(r) for r in reader]
