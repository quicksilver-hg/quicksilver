from harness.aggregate import CellStats
from harness.thresholds import DEFAULTS
from harness.verdict import evaluate_cell, overall_verdict, per_algorithm_verdict
from harness.report import render_report, render_comparison_report

def _cell(alg, create, verify, proof=168, vmem=256, params="p"):
    return CellStats(
        key=(alg, params, "cpu:test"), n=3,
        create_ms_median=create, create_ms_min=create, create_ms_max=create,
        verify_us_median=verify, verify_us_min=verify, verify_us_max=verify,
        proof_bytes=proof, verify_peak_kb=vmem, ratio=(create * 1000.0) / verify,
    )

def test_report_contains_verdict_and_table_rows():
    cells = [_cell("cuckoo", 1000.0, 10.0), _cell("equihash", 2000.0, 80.0, proof=1344)]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    decision, reasons = overall_verdict(cells, verdicts)
    md = render_report(cells, verdicts, decision, reasons, DEFAULTS)
    assert "# Tier 0 PoW Harness — Decision Report" in md
    assert decision in md
    assert "cuckoo" in md and "equihash" in md
    assert "| algorithm |" in md  # a results table header

def test_report_lists_reasons_when_rethink():
    cells = [_cell("cuckoo", 10.0, 9000.0)]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    decision, reasons = overall_verdict(cells, verdicts)
    md = render_report(cells, verdicts, decision, reasons, DEFAULTS)
    assert "RETHINK" in md
    for r in reasons:
        assert r in md

# --- render_comparison_report tests ---

def test_comparison_report_contains_algorithm_names_and_verdicts():
    """Both algorithm names and their verdicts must appear in the comparison report."""
    cells = [
        _cell("cuckatoo", 1000.0, 1.5, proof=168),
        _cell("equihash", 5000.0, 120.0, proof=2048),
    ]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    per_alg = per_algorithm_verdict(cells, verdicts)
    md = render_comparison_report(cells, verdicts, per_alg, DEFAULTS)
    assert "cuckatoo" in md
    assert "equihash" in md
    assert "PROCEED" in md
    assert "RETHINK" in md

def test_comparison_report_has_recommendation_section():
    """A Recommendation section must be present."""
    cells = [
        _cell("cuckatoo", 1000.0, 1.5, proof=168),
        _cell("equihash", 5000.0, 120.0, proof=2048),
    ]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    per_alg = per_algorithm_verdict(cells, verdicts)
    md = render_comparison_report(cells, verdicts, per_alg, DEFAULTS)
    assert "Recommendation" in md

def test_comparison_report_single_proceed_names_winner():
    """When exactly one algorithm PROCEEDs, the Recommendation must name it."""
    cells = [
        _cell("cuckatoo", 1000.0, 1.5, proof=168),
        _cell("equihash", 5000.0, 120.0, proof=2048),
    ]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    per_alg = per_algorithm_verdict(cells, verdicts)
    md = render_comparison_report(cells, verdicts, per_alg, DEFAULTS)
    # Recommendation block should name the winner
    rec_idx = md.index("## Recommendation")
    rec_section = md[rec_idx:]
    assert "cuckatoo" in rec_section

def test_comparison_report_includes_cell_table():
    """The per-cell results table header must appear (reuses existing table style)."""
    cells = [_cell("cuckatoo", 1000.0, 1.5, proof=168)]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    per_alg = per_algorithm_verdict(cells, verdicts)
    md = render_comparison_report(cells, verdicts, per_alg, DEFAULTS)
    assert "| algorithm |" in md

def test_comparison_report_no_proceed_all_rethink():
    """When no algorithm passes, Recommendation says all require RETHINK."""
    cells = [
        _cell("alg_a", 5000.0, 120.0, proof=2048),
        _cell("alg_b", 5000.0, 9000.0, proof=168),
    ]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    per_alg = per_algorithm_verdict(cells, verdicts)
    md = render_comparison_report(cells, verdicts, per_alg, DEFAULTS)
    assert "Recommendation" in md
    assert "RETHINK" in md
