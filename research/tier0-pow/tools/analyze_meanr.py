#!/usr/bin/env python3
"""Close the mean-r Volta+ gate from same-card lean+mean E29/E31 timings.

Consumes the combined CSV bench_cloud.sh builds (parse_solver_timing.py rows:
edgebits,solver,gpu,nonce,edges,dupes,solve_ms) and firms the number the shipped
mint-safety constant currently PROXIES with lean data.

Safety model (run2 report sec.4). Mint-safety is

    C <= alpha * (r / K) * S_tail        r = c29 / c31   (per-graph E29:E31 cost)

and is robust to any UNIFORM solver/hardware speedup: the mean solver accelerates
per-tx PoW (E29) and block PoW (E31) together, so the lean->mean discount CANCELS
in r. A big discount is therefore not itself a threat. The only residual risk is
a NON-UNIFORM discount — an E29-specific mean advantage that drops the true
`mean_r` below the `lean_r`=0.243 measured on Pascal. This gate measures mean_r
directly (mean.cu runs on sm_70+) and checks discount uniformity as the mechanism.

PRIMARY TRIGGER (matches shipped analyze_run2):  mean_r < 0.25  ->  retune K.
Discount magnitude is DIAGNOSTIC only; discount uniformity explains any r-shift.
"""
import argparse
import csv
import json

# Phase-3 held constants + ACTUAL shipped values (chainparams.cpp non-sandbox).
K_SHIPPED = 106          # nTxWorkCouplingK (analyze_calibration's 109 predates K->106)
C_SHIPPED = 57143        # nTxPowMint (sub-units)
S_TAIL    = 100_000_000  # 1 COIN
ALPHA     = 0.25         # designed mint-safety margin (1/alpha = 4x)
LEAN_R_RIG = 0.243       # run2 median lean r on P104 (the proxy this gate replaces)


def _load(csv_path):
    b = {}
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            b.setdefault((row["solver"], int(row["edgebits"])), []).append(
                int(row["solve_ms"]) / 1000.0)
    return {k: sorted(v) for k, v in b.items()}


def _stat(xs):
    return None if not xs else {"min": xs[0], "median": xs[len(xs) // 2], "n": len(xs)}


def analyze(csv_path):
    b = _load(csv_path)
    m29, m31 = _stat(b.get(("mean", 29))), _stat(b.get(("mean", 31)))
    l29, l31 = _stat(b.get(("lean", 29))), _stat(b.get(("lean", 31)))
    if not (m29 and m31):
        raise SystemExit("need mean E29 AND mean E31 timings for the r gate")

    # Security anchor = fastest sample (cheapest attacker); median tracks the report.
    mean_r_min = m29["min"] / m31["min"]
    mean_r_med = m29["median"] / m31["median"]
    margin = (mean_r_med / K_SHIPPED) * S_TAIL / C_SHIPPED   # designed 1/alpha = 4.0
    K_restore_4x = round(ALPHA * mean_r_med * S_TAIL / C_SHIPPED)
    retune = mean_r_med < 0.25

    out = {
        "mean_c29": m29, "mean_c31": m31, "lean_c29": l29, "lean_c31": l31,
        "mean_r_min": mean_r_min, "mean_r_median": mean_r_med, "theoretical_r": 0.25,
        "lean_r_rig_proxy": LEAN_R_RIG,
        "K_shipped": K_SHIPPED, "C_shipped": C_SHIPPED,
        "margin_current": margin, "margin_designed": 1.0 / ALPHA,
        "K_restore_4x": K_restore_4x, "retune": retune,
    }
    # Diagnostics: same-card discount at each edgebits + uniformity (the mechanism).
    if l29:
        out["discount_e29"] = l29["min"] / m29["min"]
    if l31:
        out["discount_e31"] = l31["min"] / m31["min"]
    if l29 and l31:
        out["lean_r_measured"] = l29["min"] / l31["min"]
        # >1 => mean is disproportionately faster at E29 => mean_r < lean_r (the risk).
        out["discount_uniformity"] = out["discount_e29"] / out["discount_e31"]
    return out


def _fmt_stat(s):
    return "n/a" if not s else f"min {s['min']:.3f}s  median {s['median']:.3f}s  (n={s['n']})"


def _human(v):
    lines = [
        "=== mean-r Volta+ gate ===",
        f"mean E29:  {_fmt_stat(v['mean_c29'])}",
        f"mean E31:  {_fmt_stat(v['mean_c31'])}",
        f"lean E29:  {_fmt_stat(v['lean_c29'])}",
        f"lean E31:  {_fmt_stat(v['lean_c31'])}",
        "",
        f"mean_r = {v['mean_r_median']:.3f} (median), {v['mean_r_min']:.3f} (min)"
        f"   vs theoretical 0.25, rig lean-proxy {v['lean_r_rig_proxy']}",
    ]
    if "discount_uniformity" in v:
        u = v["discount_uniformity"]
        lines += [
            f"discount: E29 {v['discount_e29']:.1f}x  E31 {v['discount_e31']:.1f}x"
            f"  uniformity {u:.2f}  ({'E29-specific mean advantage -> mean_r<lean_r' if u > 1.05 else 'uniform -> cancels in r'})",
            f"lean_r (measured, cross-check vs rig 0.243): {v['lean_r_measured']:.3f}",
        ]
    elif "discount_e29" in v:
        lines.append(f"discount E29: {v['discount_e29']:.1f}x  (add lean E31 for uniformity check)")
    if v["mean_r_min"] < 0.25 <= v["mean_r_median"]:
        lines.append("NOTE: min r < 0.25 while median >= 0.25 — cheapest-attacker bound is tighter.")
    lines += [
        "",
        f"C margin at shipped K={v['K_shipped']}: {v['margin_current']:.2f}x (designed {v['margin_designed']:.1f}x)",
        f"K to restore 4x margin for this mean_r: {v['K_restore_4x']}",
        "",
        f"VERDICT: {'RETUNE (mean_r < 0.25) -> set K=%d' % v['K_restore_4x'] if v['retune'] else 'MEASURED-AND-HELD (mean_r >= 0.25; no consensus change)'}",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="combined.csv from bench_cloud.sh")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    v = analyze(a.csv)
    print(json.dumps(v, indent=2) if a.json else _human(v))


if __name__ == "__main__":
    main()
