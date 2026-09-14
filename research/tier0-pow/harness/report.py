from __future__ import annotations
from harness.aggregate import CellStats
from harness.thresholds import Thresholds
from harness.verdict import CellVerdict

def _ok(b: bool) -> str:
    return "PASS" if b else "FAIL"

def _table_header() -> list[str]:
    return [
        "| algorithm | params | hardware | n | create ms (med) | verify us (med) | "
        "ratio | proof B | verify KB | verify? | ratio? | proof? | mem? | in-band? |",
        "|" + "---|" * 14,
    ]

def _table_row(c: CellStats, v: CellVerdict) -> str:
    alg, params, hw = c.key
    return (
        f"| {alg} | {params} | {hw} | {c.n} | {c.create_ms_median:.1f} | "
        f"{c.verify_us_median:.2f} | {c.ratio:.0f}x | {c.proof_bytes} | "
        f"{c.verify_peak_kb} | {_ok(v.verify_ok)} | {_ok(v.ratio_ok)} | "
        f"{_ok(v.proof_ok)} | {_ok(v.verify_mem_ok)} | {_ok(v.create_in_band)} |"
    )

def render_report(
    cells: list[CellStats],
    verdicts: list[CellVerdict],
    decision: str,
    reasons: list[str],
    t: Thresholds,
) -> str:
    lines: list[str] = []
    lines.append("# Tier 0 PoW Harness — Decision Report")
    lines.append("")
    lines.append(f"**Verdict: {decision}**")
    lines.append("")
    if reasons:
        lines.append("## Why")
        for r in reasons:
            lines.append(f"- {r}")
        lines.append("")
    lines.append("## Thresholds")
    lines.append(
        f"- verify ≤ {t.verify_us_max:.0f} us · ratio ≥ {t.ratio_min:.0f}x · "
        f"proof ≤ {t.proof_bytes_max} B · verify mem ≤ {t.verify_peak_kb_max} KB · "
        f"create band [{t.create_ms_min:.0f}, {t.create_ms_max:.0f}] ms"
    )
    lines.append("")
    lines.append("## Results")
    lines.extend(_table_header())
    for c, v in zip(cells, verdicts):
        lines.append(_table_row(c, v))
    lines.append("")
    return "\n".join(lines)

def render_comparison_report(
    cells: list[CellStats],
    verdicts: list[CellVerdict],
    per_alg: dict[str, tuple[str, list[str]]],
    t: Thresholds,
) -> str:
    """Render a per-algorithm comparison report.

    Shows each algorithm's verdict + reasons, its subset of cells, and an overall
    Recommendation naming algorithms that PROCEED (single best-choice highlighted).
    """
    lines: list[str] = []
    lines.append("# Tier 0 PoW Harness — Algorithm Comparison Report")
    lines.append("")
    lines.append("## Thresholds")
    lines.append(
        f"- verify ≤ {t.verify_us_max:.0f} us · ratio ≥ {t.ratio_min:.0f}x · "
        f"proof ≤ {t.proof_bytes_max} B · verify mem ≤ {t.verify_peak_kb_max} KB · "
        f"create band [{t.create_ms_min:.0f}, {t.create_ms_max:.0f}] ms"
    )
    lines.append("")

    # Pair each cell with its verdict for lookup.
    cv_pairs = list(zip(cells, verdicts))

    for alg, (decision, reasons) in per_alg.items():
        lines.append(f"## Algorithm: {alg}")
        lines.append("")
        lines.append(f"**Verdict: {decision}**")
        lines.append("")
        if reasons:
            lines.append("### Reasons")
            for r in reasons:
                lines.append(f"- {r}")
            lines.append("")
        # Per-cell table for this algorithm only.
        alg_pairs = [(c, v) for c, v in cv_pairs if c.key[0] == alg]
        if alg_pairs:
            lines.append("### Results")
            lines.extend(_table_header())
            for c, v in alg_pairs:
                lines.append(_table_row(c, v))
            lines.append("")

    # Overall recommendation.
    proceeding = [alg for alg, (decision, _) in per_alg.items() if decision == "PROCEED"]
    lines.append("## Recommendation")
    if not proceeding:
        lines.append(
            "No algorithm passed all gates. All require RETHINK before adoption."
        )
    elif len(proceeding) == 1:
        lines.append(
            f"**Adopt {proceeding[0]}** — the only algorithm that passes all gates."
        )
    else:
        alg_list = ", ".join(proceeding)
        lines.append(
            f"The following algorithms pass all gates: {alg_list}. "
            "Choose based on secondary criteria (GPU friendliness, tooling maturity, "
            "community support)."
        )
    lines.append("")
    return "\n".join(lines)
