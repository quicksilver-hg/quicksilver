from harness.aggregate import CellStats
from harness.thresholds import DEFAULTS
from harness.verdict import evaluate_cell, overall_verdict, per_algorithm_verdict

def _cell(alg="cuckoo", params="p", create=1000.0, verify=10.0, proof=168, vmem=256):
    return CellStats(
        key=(alg, params, "cpu:test"), n=3,
        create_ms_median=create, create_ms_min=create, create_ms_max=create,
        verify_us_median=verify, verify_us_min=verify, verify_us_max=verify,
        proof_bytes=proof, verify_peak_kb=vmem,
        ratio=(create * 1000.0) / verify,
    )

def test_healthy_cell_passes_all_correctness_checks():
    v = evaluate_cell(_cell(), DEFAULTS)
    assert v.verify_ok and v.ratio_ok and v.proof_ok and v.verify_mem_ok
    assert v.create_in_band

def test_expensive_verify_fails():
    v = evaluate_cell(_cell(verify=5000.0), DEFAULTS)  # 5 ms verify
    assert not v.verify_ok

def test_oversized_proof_fails():
    v = evaluate_cell(_cell(proof=2048), DEFAULTS)
    assert not v.proof_ok

def test_low_ratio_fails():
    # create=1ms, verify=10us -> ratio = 100x, below the 1000x floor
    v = evaluate_cell(_cell(create=1.0, verify=10.0), DEFAULTS)
    assert not v.ratio_ok

def test_excessive_verify_memory_fails():
    v = evaluate_cell(_cell(vmem=131072), DEFAULTS)  # 128 MB > 64 MB cap
    assert not v.verify_mem_ok

def test_overall_proceeds_when_all_correct_and_floor_exists():
    cells = [_cell(create=50.0), _cell(create=1000.0)]  # 2nd is in band
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    decision, reasons = overall_verdict(cells, verdicts)
    assert decision == "PROCEED"
    assert reasons == []

def test_overall_rethinks_when_any_cell_verify_too_costly():
    cells = [_cell(create=1000.0), _cell(verify=9000.0)]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    decision, reasons = overall_verdict(cells, verdicts)
    assert decision == "RETHINK"
    assert any("verify" in r for r in reasons)

def test_overall_rethinks_when_no_usable_floor():
    cells = [_cell(create=10.0), _cell(create=20.0)]  # all too fast to rate-limit
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    decision, reasons = overall_verdict(cells, verdicts)
    assert decision == "RETHINK"
    assert any("floor" in r for r in reasons)

# --- per_algorithm_verdict tests ---

def test_per_algorithm_verdict_cuckatoo_all_pass():
    """Cuckatoo cells all passing gates → PROCEED."""
    cells = [
        _cell(alg="cuckatoo", params="edgebits19", create=1000.0, verify=1.5),
        _cell(alg="cuckatoo", params="edgebits19", create=1200.0, verify=1.8),
    ]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    result = per_algorithm_verdict(cells, verdicts)
    assert "cuckatoo" in result
    decision, reasons = result["cuckatoo"]
    assert decision == "PROCEED"
    assert reasons == []

def test_per_algorithm_verdict_equihash_large_proof_rethink():
    """Equihash cell with proof_bytes=2048 → RETHINK with a proof reason."""
    cells = [_cell(alg="equihash", params="n200k9", create=5000.0, verify=120.0, proof=2048)]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    result = per_algorithm_verdict(cells, verdicts)
    assert "equihash" in result
    decision, reasons = result["equihash"]
    assert decision == "RETHINK"
    assert any("proof" in r for r in reasons)

def test_per_algorithm_verdict_both_present_independent():
    """Mixed: cuckatoo passes, equihash fails proof. Both present; verdicts are independent."""
    cells = [
        _cell(alg="cuckatoo", params="edgebits19", create=1000.0, verify=1.5),
        _cell(alg="equihash", params="n200k9", create=5000.0, verify=120.0, proof=2048),
    ]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    result = per_algorithm_verdict(cells, verdicts)
    assert set(result.keys()) == {"cuckatoo", "equihash"}
    cuck_decision, _ = result["cuckatoo"]
    eq_decision, eq_reasons = result["equihash"]
    assert cuck_decision == "PROCEED"
    assert eq_decision == "RETHINK"
    assert any("proof" in r for r in eq_reasons)

def test_per_algorithm_verdict_equihash_bad_proof_does_not_affect_cuckatoo():
    """Equihash failure must not bleed into cuckatoo's verdict."""
    cells = [
        _cell(alg="cuckatoo", params="edgebits19", create=1000.0, verify=1.5),
        _cell(alg="equihash", params="n200k9", create=5000.0, verify=120.0, proof=2048),
    ]
    verdicts = [evaluate_cell(c, DEFAULTS) for c in cells]
    result = per_algorithm_verdict(cells, verdicts)
    cuck_decision, cuck_reasons = result["cuckatoo"]
    assert cuck_decision == "PROCEED", (
        f"cuckatoo should PROCEED independently; got {cuck_decision}, reasons={cuck_reasons}"
    )
