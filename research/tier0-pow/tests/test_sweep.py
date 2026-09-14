import textwrap
from pathlib import Path
from harness.sweep import DriverSpec, run_sweep

def _fake_driver(tmp_path: Path) -> Path:
    # Prints one CSV data row echoing its param/hardware args (no header).
    script = tmp_path / "fake_driver.py"
    script.write_text(textwrap.dedent("""
        import sys
        param, hardware, runs, loops = sys.argv[1:5]
        for i in range(int(runs)):
            print(f"cuckoo,{param},{hardware},{i},1000.0,10.0,168,256,500000")
    """))
    return script

def test_run_sweep_collects_rows_across_params(tmp_path):
    fake = _fake_driver(tmp_path)
    spec = DriverSpec(
        algorithm="cuckoo",
        command=["python3", str(fake)],
        param_labels=["edgebits=29", "edgebits=30"],
        hardware_label="cpu:test",
        runs=2,
        verify_loops=1000,
    )
    rows = run_sweep([spec])
    # 2 params * 2 runs = 4 rows
    assert len(rows) == 4
    assert {r.param_label for r in rows} == {"edgebits=29", "edgebits=30"}
    assert all(r.hardware_label == "cpu:test" for r in rows)
