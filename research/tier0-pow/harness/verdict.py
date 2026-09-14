from __future__ import annotations
from dataclasses import dataclass
from harness.aggregate import CellStats
from harness.thresholds import Thresholds

@dataclass(frozen=True)
class CellVerdict:
    verify_ok: bool
    ratio_ok: bool
    proof_ok: bool
    verify_mem_ok: bool
    create_in_band: bool

def evaluate_cell(c: CellStats, t: Thresholds) -> CellVerdict:
    return CellVerdict(
        verify_ok=c.verify_us_median <= t.verify_us_max,
        ratio_ok=c.ratio >= t.ratio_min,
        proof_ok=c.proof_bytes <= t.proof_bytes_max,
        verify_mem_ok=c.verify_peak_kb <= t.verify_peak_kb_max,
        create_in_band=t.create_ms_min <= c.create_ms_median <= t.create_ms_max,
    )

def overall_verdict(
    cells: list[CellStats], verdicts: list[CellVerdict]
) -> tuple[str, list[str]]:
    reasons: list[str] = []
    for c, v in zip(cells, verdicts):
        label = ":".join(c.key)
        if not v.verify_ok:
            reasons.append(f"{label}: verify cost {c.verify_us_median:.1f}us too high")
        if not v.ratio_ok:
            reasons.append(f"{label}: create:verify ratio {c.ratio:.0f}x too low")
        if not v.proof_ok:
            reasons.append(f"{label}: proof {c.proof_bytes}B too large")
        if not v.verify_mem_ok:
            reasons.append(f"{label}: verify memory {c.verify_peak_kb}KB too high")
    if not any(v.create_in_band for v in verdicts):
        reasons.append("no usable rate-limiting floor: no cell's create time is in band")
    return ("PROCEED" if not reasons else "RETHINK", reasons)

def per_algorithm_verdict(
    cells: list[CellStats], verdicts: list[CellVerdict]
) -> dict[str, tuple[str, list[str]]]:
    """Group cells by algorithm (cell.key[0]) and return overall_verdict for each group.

    Returns a dict mapping algorithm name -> (decision, reasons).
    """
    # Build per-algorithm groups preserving order of first appearance.
    groups: dict[str, tuple[list[CellStats], list[CellVerdict]]] = {}
    for c, v in zip(cells, verdicts):
        alg = c.key[0]
        if alg not in groups:
            groups[alg] = ([], [])
        groups[alg][0].append(c)
        groups[alg][1].append(v)

    return {
        alg: overall_verdict(alg_cells, alg_verdicts)
        for alg, (alg_cells, alg_verdicts) in groups.items()
    }
