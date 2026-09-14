#!/usr/bin/env python3
"""Analyze edgebits-29 per-tx PoW calibration samples for #5c.

Reads the combined CSV (solve_index,attempts,wall_ms,gpu_model,solver) produced
by calibrate_txpow.py and reports the two distributions that matter:

  attempts  -- intrinsic per-cycle difficulty (hardware-independent; a draw from
               a geometric distribution, expected mean ~42 since a random E29
               graph holds a 42-cycle with prob ~1/42). This anchors the
               *txwork unit*.
  wall_ms   -- ground-truth wall cost to mint one tx-PoW on the measured solver
               (lean) / hardware (P104-100). This anchors the baseline C, and
               we calibrate C to the FASTEST observed sample (security-
               conservative: assume the cheapest attacker).

Usage: analyze_calibration.py <combined.csv>
"""
import csv
import statistics as st
import sys


def pct(xs, p):
    # linear-interpolation percentile on a sorted copy
    xs = sorted(xs)
    if not xs:
        return float("nan")
    if len(xs) == 1:
        return float(xs[0])
    k = (len(xs) - 1) * (p / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def describe(name, xs, unit):
    print(f"\n== {name} ({unit}) ==")
    print(f"  n        {len(xs)}")
    print(f"  min      {min(xs):.3f}")
    print(f"  p10      {pct(xs,10):.3f}")
    print(f"  p25      {pct(xs,25):.3f}")
    print(f"  median   {pct(xs,50):.3f}")
    print(f"  mean     {st.mean(xs):.3f}")
    print(f"  p75      {pct(xs,75):.3f}")
    print(f"  p90      {pct(xs,90):.3f}")
    print(f"  p99      {pct(xs,99):.3f}")
    print(f"  max      {max(xs):.3f}")
    print(f"  stdev    {st.pstdev(xs):.3f}")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "calibration/run1/combined.csv"
    attempts, wall_ms = [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            attempts.append(int(row["attempts"]))
            wall_ms.append(float(row["wall_ms"]))

    describe("attempts-to-land-one-42-cycle", attempts, "graphs")
    describe("wall time per minted tx-PoW", [w / 1000.0 for w in wall_ms], "seconds")

    # per-attempt wall cost (each sample = ~one CUDA-context init + N graph solves)
    per_attempt = [wall_ms[i] / attempts[i] / 1000.0 for i in range(len(attempts))]
    describe("derived per-attempt (graph) wall cost", per_attempt, "seconds")

    # Security-conservative anchors: the cheapest cycle an attacker could buy.
    fastest_cycle_s = min(w / 1000.0 for w in wall_ms)
    fastest_attempt_s = min(per_attempt)
    print("\n== security-conservative anchors (fastest observed) ==")
    print(f"  fastest full cycle      {fastest_cycle_s:.1f} s")
    print(f"  fastest per-attempt     {fastest_attempt_s:.3f} s")
    print(f"  geometric p(cycle/graph) ~= {1.0/ (st.mean(attempts)):.4f} "
          f"(mean attempts={st.mean(attempts):.1f}, theory ~42)")


# --- run2 (sub-project A): lean-vs-lean r ratio + mint-safety margin -----------
# Pivot 2026-06-28: the mean solver is unusable on the rig's Pascal P104 (memory
# `mean-solver-pascal-incompat`), so r = c29/c31 is measured lean-vs-lean (a valid
# same-solver ratio; spec §5 branch 1). The lean->mean discount (~4x, Tromp) is
# uniform across edgebits, so it cancels in r and is closed from literature, not
# the rig. Held constants from the Phase-3 calibration (G=2, alpha=0.25, N=3500):
RUN2_K_CURRENT = 109            # shipped nTxWorkCouplingK
RUN2_C = 57143                  # shipped nTxPowMint (sub-units)
RUN2_S_TAIL = 100_000_000       # 1 COIN
RUN2_ALPHA = 0.25               # designed 4x mint-safety margin
RUN2_THEORETICAL_R = 0.25       # edge-count estimate (E29 = 1/4 of E31)


def analyze_run2(csv_path):
    """Read lean_timing.csv (edgebits,gpu,nonce,time_ms) and return the r/margin/
    retune verdict for the mint-safety inequality C <= (r/K)*S_tail."""
    import csv as _csv
    e = {29: [], 31: []}
    with open(csv_path) as f:
        for row in _csv.DictReader(f):
            t = row["time_ms"].strip()
            if t.isdigit():
                e[int(row["edgebits"])].append(int(t) / 1000.0)
    c29, c31 = sorted(e[29]), sorted(e[31])
    c29_med, c31_med = pct(c29, 50), pct(c31, 50)
    r_median = c29_med / c31_med
    r_min = c29[0] / c31[0]                       # tight distributions -> ~r_median
    # margin = how far C sits under the ceiling (r/K)*S_tail, at the shipped K.
    margin_current = (r_median / RUN2_K_CURRENT) * RUN2_S_TAIL / RUN2_C
    # K that puts C back at exactly the designed alpha (=4x) margin for this r:
    #   target C = alpha*(r/K)*S_tail  =>  K = alpha*r*S_tail/C
    K_restore_4x = round(RUN2_ALPHA * r_median * RUN2_S_TAIL / RUN2_C)
    return {
        "c29_min_s": c29[0], "c29_median_s": c29_med,
        "c31_min_s": c31[0], "c31_median_s": c31_med,
        "r_median": r_median, "r_min": r_min,
        "theoretical_r": RUN2_THEORETICAL_R,
        "margin_current": margin_current,        # designed = 1/alpha = 4.0
        "K_restore_4x": K_restore_4x,
        "retune": r_median < RUN2_THEORETICAL_R,  # spec §2 strict trigger
    }


if __name__ == "__main__":
    main()
