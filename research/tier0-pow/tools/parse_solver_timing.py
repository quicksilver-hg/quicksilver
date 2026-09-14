#!/usr/bin/env python3
"""Parse Tromp cuckatoo lean/mean solver stdout into per-graph timing rows, and
gate against the Pascal-corruption signature (memory `mean-solver-pascal-incompat`).

Solver stdout is one block per nonce, pinned to the captured format in
research/tier0-pow/tools/calibration/mean_output_sample_e29.txt:

    nonce <N> k0 k1 k2 k3 <hex> <hex> <hex> <hex>
    31 trims <a> ms <edges> edges <dupes> dupes <b> ms total <t> ms
    Time: <T> ms
    ...
    <M> total solutions

`Time: <T> ms` is the per-graph solve wall the solver self-reports; it is
preimage-independent and is the quantity C is anchored against. lean.cu emits the
same `nonce N ... Time: X ms` format, so this parser serves both solvers.

A corrupt run (mean on Pascal sm_61) collapses EVERY graph to
`... 700 edges 699 dupes ... Time: 0 ms` and ends `0 total solutions`. Trusting
those timings would falsely conclude "mean is slow, C is safe". `--gate` detects
that signature and exits non-zero BEFORE any timing is written or trusted.
"""
import argparse
import csv
import os
import re
import sys

NONCE_RE = re.compile(r"^nonce\s+(\d+)\b")
TRIM_RE  = re.compile(r"(\d+)\s+edges\s+(\d+)\s+dupes")
TIME_RE  = re.compile(r"^Time:\s+(\d+)\s*ms")
SOLS_RE  = re.compile(r"^(\d+)\s+total solutions")

CSV_FIELDS = ["edgebits", "solver", "gpu", "nonce", "edges", "dupes", "solve_ms"]


def parse(text, edgebits, solver, gpu):
    """-> (rows, total_solutions). rows have all CSV_FIELDS; only graphs that
    reported a `Time:` line are kept."""
    rows, cur, total_solutions = [], None, None
    for line in text.splitlines():
        m = NONCE_RE.match(line)
        if m:
            if cur:
                rows.append(cur)
            cur = {"edgebits": edgebits, "solver": solver, "gpu": gpu,
                   "nonce": int(m.group(1)), "edges": None, "dupes": None,
                   "solve_ms": None}
            continue
        if cur is not None:
            mt = TRIM_RE.search(line)
            if mt:
                cur["edges"], cur["dupes"] = int(mt.group(1)), int(mt.group(2))
            mtime = TIME_RE.match(line)
            if mtime:
                cur["solve_ms"] = int(mtime.group(1))
        msol = SOLS_RE.match(line)
        if msol:
            total_solutions = int(msol.group(1))
    if cur:
        rows.append(cur)
    rows = [r for r in rows if r["solve_ms"] is not None]
    return rows, total_solutions


def corruption_report(rows, total_solutions):
    """-> (ok: bool, reason: str). Fails on the exact Pascal mean-corruption tell
    so a degraded GPU can never feed the mint-safety constant."""
    n = len(rows)
    if n == 0:
        return False, "no parseable graphs in solver output"
    times = sorted(r["solve_ms"] for r in rows)
    median = times[n // 2]
    near_dupes = sum(1 for r in rows
                     if r["edges"] and r["dupes"] is not None
                     and r["dupes"] >= r["edges"] - 2)
    frac_dupes = near_dupes / n
    # ~1/42 of graphs carry a 42-cycle; 0 in >=210 graphs is p<1% by chance.
    if total_solutions == 0 and n >= 210:
        return False, (f"0 cycles found in {n} graphs (expected ~{n // 42}); "
                       "Pascal-style corrupt trim — mean solver unusable on this GPU")
    if frac_dupes > 0.8:
        return False, (f"{frac_dupes:.0%} of graphs have dupes>=edges "
                       "(the 699/700 collapse) — corrupt trimming, timings invalid")
    if median < 1:
        return False, (f"median solve = {median} ms (~0) — no real trimming work "
                       "(finer timer needed, or corrupt)")
    return True, (f"healthy: {n} graphs, median {median} ms, min {times[0]} ms, "
                  f"{total_solutions} cycles (~{n // 42} expected)")


def _read(path):
    with open(path) as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile", help="solver stdout capture")
    ap.add_argument("--edgebits", type=int, required=True)
    ap.add_argument("--solver", required=True, choices=["mean", "lean"])
    ap.add_argument("--gpu", type=int, default=0)
    ap.add_argument("--gate", action="store_true",
                    help="check corruption signature; exit 1 if corrupt")
    ap.add_argument("--to-csv", metavar="CSV",
                    help="append parsed rows to CSV (writes header if new)")
    a = ap.parse_args()

    rows, total = parse(_read(a.logfile), a.edgebits, a.solver, a.gpu)
    ok, reason = corruption_report(rows, total)
    print(f"[{a.solver} E{a.edgebits} gpu{a.gpu}] {reason}", file=sys.stderr)

    if a.to_csv and ok:
        new = not os.path.exists(a.to_csv)
        with open(a.to_csv, "a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
            if new:
                w.writeheader()
            for r in rows:
                w.writerow(r)
        print(f"  -> +{len(rows)} rows to {a.to_csv}", file=sys.stderr)

    if a.gate and not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
