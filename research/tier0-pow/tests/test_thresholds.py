from harness.thresholds import Thresholds, DEFAULTS


def test_defaults_match_spec_section_5():
    assert DEFAULTS.verify_us_max == 1000.0       # verify ≪ ~1 ms
    assert DEFAULTS.ratio_min == 1000.0           # ≥ ~1000×
    assert DEFAULTS.proof_bytes_max == 200        # tens-to-low-hundreds
    assert DEFAULTS.create_ms_min == 250.0        # rate-limiting floor
    assert DEFAULTS.create_ms_max == 60000.0      # sanity upper bound
    assert DEFAULTS.verify_peak_kb_max == 65536   # negligible verify memory


def test_thresholds_are_overridable():
    t = Thresholds(verify_us_max=500.0)
    assert t.verify_us_max == 500.0
    assert t.ratio_min == DEFAULTS.ratio_min      # untouched fields keep defaults
