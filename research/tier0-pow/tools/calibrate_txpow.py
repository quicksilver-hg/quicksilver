#!/usr/bin/env python3
"""Drive an external Cuckatoo solver to measure edgebits-29 per-tx PoW cost.

Emits CSV: solve_index,attempts,wall_ms,gpu_model,solver
Each run uses a fresh deterministic preimage so timings are independent samples.
The marginal cost of one 42-cycle is the #5c calibration input (sets the txwork
unit and the congestion baseline C).
"""
import argparse
import csv
import subprocess
import sys
import time


def make_preimage(i: int, nbytes: int = 96) -> str:
    # Deterministic, distinct per sample; last 4 bytes are the nonce slot.
    body = bytes(((i * 131 + j * 17) & 0xff) for j in range(nbytes))
    return body.hex()


def run_once(solver: str, edgebits: int, hexpre: str, max_attempts: int):
    t0 = time.perf_counter()
    out = subprocess.run([solver, str(edgebits), hexpre, "1", str(max_attempts)],
                         capture_output=True, text=True)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    nonce = None
    for line in out.stdout.splitlines():
        if line.startswith("nonce="):
            nonce = int(line[6:])
    # attempts to land the cycle = nonce - start + 1 (start is 1), else max_attempts
    attempts = nonce if nonce is not None else max_attempts
    return (attempts, wall_ms, nonce is not None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--solver", required=True)
    ap.add_argument("--edgebits", type=int, default=29)
    ap.add_argument("--samples", type=int, default=200)
    ap.add_argument("--start-index", type=int, default=0,
                    help="First preimage index; lets parallel workers take "
                         "disjoint slices so samples stay independent.")
    ap.add_argument("--max-attempts", type=int, default=100000)
    ap.add_argument("--gpu-model", required=True)
    ap.add_argument("--solver-name", default="lean")
    ap.add_argument("--out", default="txpow_calibration.csv")
    a = ap.parse_args()

    with open(a.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["solve_index", "attempts", "wall_ms", "gpu_model", "solver"])
        found = 0
        for k in range(a.samples):
            i = a.start_index + k
            attempts, wall_ms, ok = run_once(a.solver, a.edgebits,
                                             make_preimage(i), a.max_attempts)
            if ok:
                found += 1
                w.writerow([i, attempts, f"{wall_ms:.1f}", a.gpu_model, a.solver_name])
            f.flush()
            print(f"[idx={i} {k + 1}/{a.samples}] solved={ok} attempts={attempts} "
                  f"wall_ms={wall_ms:.1f}", file=sys.stderr, flush=True)
        print(f"{found}/{a.samples} solved -> {a.out}", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
