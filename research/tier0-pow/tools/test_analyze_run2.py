"""TDD for analyze_calibration.analyze_run2 (sub-project A, lean-vs-lean pivot).

Synthetic lean_timing.csv with known answers pins the r/margin/K math that feeds
the consensus retune decision. Format: edgebits,gpu,nonce,time_ms.
"""
import csv
import os
import tempfile
import analyze_calibration as a


def _csv(rows):
    fd, path = tempfile.mkstemp(suffix=".csv")
    os.close(fd)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["edgebits", "gpu", "nonce", "time_ms"])
        w.writeheader()
        for r in rows:
            w.writerow(r)
    return path


def test_r_margin_and_retune_target():
    # E29 median 4.0 s, E31 median 16.0 s -> r = 0.25 exactly (margin == design 4x, no retune).
    rows = []
    rows += [{"edgebits": 29, "gpu": 0, "nonce": i, "time_ms": 4000} for i in range(5)]
    rows += [{"edgebits": 31, "gpu": 0, "nonce": i, "time_ms": 16000} for i in range(5)]
    res = a.analyze_run2(_csv(rows))
    assert abs(res["c29_median_s"] - 4.0) < 1e-6
    assert abs(res["c31_median_s"] - 16.0) < 1e-6
    assert abs(res["r_median"] - 0.25) < 1e-6
    # margin = (r/K)*S_tail / C ; K=109, C=57143, S_tail=1e8
    # = (0.25/109)*1e8/57143 = 4.012x
    assert abs(res["margin_current"] - 4.012) < 0.01
    assert res["retune"] is False           # r >= 0.25
    # K that restores exactly 4x at this r: round(alpha*r*S_tail/C) = round(0.25*0.25*1e8/57143)=109
    assert res["K_restore_4x"] == 109


def test_r_below_quarter_trips_retune():
    # E29 median 4.86 s, E31 median 20.0 s -> r = 0.243 (< 0.25 -> retune); margin ~3.90x
    rows = []
    rows += [{"edgebits": 29, "gpu": 0, "nonce": i, "time_ms": 4860} for i in range(5)]
    rows += [{"edgebits": 31, "gpu": 0, "nonce": i, "time_ms": 20000} for i in range(5)]
    res = a.analyze_run2(_csv(rows))
    assert abs(res["r_median"] - 0.243) < 1e-3
    assert res["retune"] is True            # 0.243 < 0.25
    assert res["margin_current"] < 4.0 and res["margin_current"] > 3.8
    # K to restore 4x at r=0.243: round(0.25*0.243*1e8/57143) = round(106.3) = 106
    assert res["K_restore_4x"] == 106
