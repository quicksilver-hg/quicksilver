from __future__ import annotations
from dataclasses import dataclass


@dataclass(frozen=True)
class Thresholds:
    # Load-bearing correctness checks (must hold for EVERY cell):
    verify_us_max: float = 1000.0       # absolute verify cost on CPU
    ratio_min: float = 1000.0           # create:verify asymmetry
    proof_bytes_max: int = 200          # per-tx on-chain overhead
    verify_peak_kb_max: int = 65536     # verify must be memory-cheap (64 MB)
    # Tuning band (a usable floor cell must land in this create-time range):
    create_ms_min: float = 250.0
    create_ms_max: float = 60000.0


DEFAULTS = Thresholds()
