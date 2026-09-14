import io
import pytest
from harness.results import Result, parse_row, load_results

HEADER = "algorithm,param_label,hardware_label,run_index,create_ms,verify_us,proof_bytes,verify_peak_kb,create_peak_kb"

def test_parse_row_builds_typed_result():
    row = {
        "algorithm": "cuckoo", "param_label": "edgebits=29", "hardware_label": "cpu:test",
        "run_index": "0", "create_ms": "1234.5", "verify_us": "12.3",
        "proof_bytes": "168", "verify_peak_kb": "256", "create_peak_kb": "524288",
    }
    r = parse_row(row)
    assert r == Result("cuckoo", "edgebits=29", "cpu:test", 0, 1234.5, 12.3, 168, 256, 524288)

def test_load_results_reads_all_rows():
    csv_text = HEADER + "\n" \
        "cuckoo,edgebits=29,cpu:test,0,1000.0,10.0,168,256,500000\n" \
        "cuckoo,edgebits=29,cpu:test,1,1100.0,11.0,168,256,500000\n"
    rows = load_results(io.StringIO(csv_text))
    assert len(rows) == 2
    assert rows[1].create_ms == 1100.0

def test_load_results_rejects_unknown_columns():
    bad = "algorithm,bogus\ncuckoo,x\n"
    with pytest.raises(ValueError):
        load_results(io.StringIO(bad))
