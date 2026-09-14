# M2b — CPU/GPU solver agreement

The plan asked for this at E22 only, in a file named `agreement-e22.md`. It is
recorded here across **all eight swept sizes** instead, because the CPU sweep
(M1) and the GPU sweep (M2) were deliberately given the same pre-image, which
makes every size comparable at no extra cost.

## What is being checked, and why counts would not be enough

M2 measures the cycle rate on GPU and M1 measures it on CPU. The GPU result is
only transferable if cycle presence is a property of the *graph* rather than of
the solver. A trimming solver that silently missed some cycles and found others
would produce a perfectly normal-looking rate — the same count, from a different
set of graphs.

So the comparison is over the **set** of nonces that yielded a cycle, not the
number. Both harnesses key their graphs from the identical 80-byte pre-image
(`"quicksilver-calibration-vector"`, zero-padded, nonce in the trailing four
little-endian bytes), so a CPU row and a GPU row bearing the same nonce describe
the same graph. The comparison is restricted to the nonce range both sweeps
covered.

## Result: identical at every size

| Edge bits | Nonces compared | CPU cycles | GPU cycles | Agreed | CPU-only | GPU-only | Verdict |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 22 | 1300 | 28 | 28 | 28 | 0 | 0 | IDENTICAL |
| 23 | 1300 | 29 | 29 | 29 | 0 | 0 | IDENTICAL |
| 24 | 1300 | 31 | 31 | 31 | 0 | 0 | IDENTICAL |
| 25 | 500 | 12 | 12 | 12 | 0 | 0 | IDENTICAL |
| 26 | 500 | 7 | 7 | 7 | 0 | 0 | IDENTICAL |
| 27 | 200 | 3 | 3 | 3 | 0 | 0 | IDENTICAL |
| 28 | 200 | 6 | 6 | 6 | 0 | 0 | IDENTICAL |
| 29 | 200 | 4 | 4 | 4 | 0 | 0 | IDENTICAL |
| **total** | **4200** | **120** | **120** | **120** | **0** | **0** | **IDENTICAL** |

**Verdict: M2 transfers.** 120 cycle events, two independent solver families
(`lean.cpp` on 8 CPU threads, `lean.cu` on a P102-100), zero disagreement in
either direction. Cycle presence is a property of the graph, so the GPU-measured
rate applies to the CPU and the two sweeps can be pooled — which
`m2-cycle-rate/README.md` does.

This is a stronger result than the plan required. The plan's fallback was that if
the sets differed, the cycle rate would have to be measured on CPU per size at
roughly 20 hours for E29 alone. That fallback is not needed.

## What this does not establish

Agreement on *which* graphs hold a cycle does not establish agreement on *which*
cycle is reported when a graph holds more than one. Both solvers report only the
first solution found, and the orderings need not match. That does not affect any
Stage 1 conclusion — the rate and the timings are both indifferent to which of
several valid cycles is returned — but it is the reason the check is framed as
set agreement over nonces rather than equality of the reported proofs.

Cycle *validity* is covered separately and completely: all 277 GPU cycles
re-verify on the CPU, 65 of them under the consensus verifier.
