from harness.results import Result
from harness.aggregate import aggregate

def _r(run, create_ms, verify_us):
    return Result("cuckoo", "edgebits=29", "cpu:test", run, create_ms, verify_us, 168, 256, 500000)

def test_aggregate_groups_and_summarizes():
    rows = [_r(0, 1000.0, 10.0), _r(1, 1200.0, 14.0), _r(2, 1100.0, 12.0)]
    cells = aggregate(rows)
    assert len(cells) == 1
    c = cells[0]
    assert c.key == ("cuckoo", "edgebits=29", "cpu:test")
    assert c.n == 3
    assert c.create_ms_median == 1100.0
    assert c.verify_us_median == 12.0
    assert c.create_ms_min == 1000.0
    assert c.create_ms_max == 1200.0
    assert c.proof_bytes == 168
    assert c.verify_peak_kb == 256
    assert round(c.ratio) == round((1100.0 * 1000.0) / 12.0)

def test_aggregate_separates_distinct_cells():
    rows = [
        Result("cuckoo", "edgebits=29", "cpu:test", 0, 1000.0, 10.0, 168, 256, 1),
        Result("equihash", "n=200,k=9", "cpu:test", 0, 2000.0, 50.0, 1344, 1024, 1),
    ]
    cells = aggregate(rows)
    assert {c.key[0] for c in cells} == {"cuckoo", "equihash"}
