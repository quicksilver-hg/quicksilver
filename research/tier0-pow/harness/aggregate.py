from __future__ import annotations
from dataclasses import dataclass
from statistics import median
from harness.results import Result

@dataclass(frozen=True)
class CellStats:
    key: tuple[str, str, str]          # (algorithm, param_label, hardware_label)
    n: int
    create_ms_median: float
    create_ms_min: float
    create_ms_max: float
    verify_us_median: float
    verify_us_min: float
    verify_us_max: float
    proof_bytes: int
    verify_peak_kb: int
    ratio: float                       # (create_ms_median scaled ms->us) / verify_us_median; dimensionless

def aggregate(rows: list[Result]) -> list[CellStats]:
    groups: dict[tuple[str, str, str], list[Result]] = {}
    for r in rows:
        groups.setdefault((r.algorithm, r.param_label, r.hardware_label), []).append(r)

    cells: list[CellStats] = []
    for key, rs in groups.items():
        creates = [r.create_ms for r in rs]
        verifies = [r.verify_us for r in rs]
        cm = median(creates)
        vm = median(verifies)
        cells.append(CellStats(
            key=key,
            n=len(rs),
            create_ms_median=cm,
            create_ms_min=min(creates),
            create_ms_max=max(creates),
            verify_us_median=vm,
            verify_us_min=min(verifies),
            verify_us_max=max(verifies),
            proof_bytes=rs[0].proof_bytes,
            verify_peak_kb=rs[0].verify_peak_kb,
            ratio=(cm * 1000.0) / vm if vm > 0 else float("inf"),
        ))
    return cells
