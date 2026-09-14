# M2 — cycle rate per graph on GPU, edge bits 22–29 (plus 19)

- Date: 2026-08-01
- Git commit: `d4a9b38`
- GPU: NVIDIA P102-100, 10 GB, compute capability 6.1 (`sm_61`), driver 580.142
- Host: P102-100 calibration host, 4 cores, 15 GiB RAM, Linux 6.8.0-136
- Built with: nvcc 12.0.140, `-arch sm_61`
- Command: `tools/calibration/run-m2.sh ~/qscal ~/qscal/out 1300`
- Produced by: `src/crypto/cuckatoo/gpu/qsgpucalibrate.cu`
- Runtime: 12:58 → 14:36 UTC (1 h 38 m), 11,700 graphs

## Why the harness is not the one the plan named

The plan ran `qscalibrate` with `CUCKATOO_GPU_SOLVER` set. That does not select a
GPU solver. `qscalibrate` resolves its solver through the bench registry, which
holds only CPU (`lean.cpp`) instantiations, and contains no `getenv` at all; the
variable is honoured solely by the node's mining bridge in `gpu_solver.cpp`.
Following the plan literally would have written **CPU timings into files named
`gpu-e*.csv`** — well-formed, with a correct cycle rate, and wrong by ~20× in the
one column M3 exists to measure.

`qsgpucalibrate.cu` is the CUDA twin of `qscalibrate`: same pre-image, same nonce
placement, same columns, one row per graph. Sharing the pre-image is what makes
the agreement check below meaningful.

## Compile host and run host differ

There is no CUDA toolkit on the P102-100 calibration host and no GPU on the build
machine. Binaries are compiled with nvcc 12.0 for `sm_61` on the build machine
and run on the calibration host. `nvcc` links cudart statically by default, so
the produced binary needs only the calibration host's
driver — `ldd` shows no CUDA dependency. Driver 580.142 is far newer than CUDA
12.0 requires, so the pairing is supported in the normal direction.

## Verification gate: PASSED

Every cycle was re-checked on the CPU before any timing was used, via
`qscalibrate --verify-gpu-csv`. The GPU solver does verify internally before
recording a solution (`vendor/lean.cu:357`), but that is the same code family
that found it; a newly built graph size is exactly where a solver defect would
hide, and a timing sweep over a broken solver looks perfectly healthy.

| Edge bits | Graphs | Cycles found | Verified on CPU | Checked by consensus | Failed |
| --- | ---: | ---: | ---: | ---: | ---: |
| 19 | 1300 | 33 | 33 | **33** | 0 |
| 22 | 1300 | 28 | 28 | — | 0 |
| 23 | 1300 | 29 | 29 | — | 0 |
| 24 | 1300 | 31 | 31 | — | 0 |
| 25 | 1300 | 36 | 36 | — | 0 |
| 26 | 1300 | 33 | 33 | — | 0 |
| 27 | 1300 | 26 | 26 | — | 0 |
| 28 | 1300 | 29 | 29 | — | 0 |
| 29 | 1300 | 32 | 32 | **32** | 0 |
| **total** | **11700** | **277** | **277** | **65** | **0** |

E19 and E29 are the only sizes where `CuckatooVerify` dispatches, so 65 of the
277 cycles were checked against the code the network actually runs. None failed.

## Result: the cycle rate is a constant, and it is 1/PROOFSIZE

Pooled over all sizes: **277 / 11,700 = 0.02368**, 95% CI [0.02092, 0.02643].
Chi-square against the pooled rate is **2.45 on 8 dof** (critical 15.51 at
p = 0.05) — the per-size variation is entirely sampling noise.

Combined with M1's CPU sweep, which found the same independence:

| Source | Cycles | Graphs | Rate | Graphs per cycle |
| --- | ---: | ---: | ---: | ---: |
| M1 (CPU, 8 threads) | 174 | 7,600 | 0.02289 | 43.7 |
| M2 (GPU) | 277 | 11,700 | 0.02368 | 42.2 |
| **combined** | **451** | **19,300** | **0.02337** | **42.8** [39.2, 47.1] |

The combined 95% CI is [0.02124, 0.02550], which **contains 1/42 = 0.023810**.
The measured rate is statistically indistinguishable from one 42-cycle per
`PROOFSIZE` graphs — the theoretical expectation for Cuckatoo at an edge/node
ratio of 1/2.

Two consequences for the flag day. The cost of a candidate `nTxEdgeBits` is its
per-graph time multiplied by a constant 42.8, so the size can be chosen from the
timing table alone. And because the constant is a property of `PROOFSIZE` rather
than of the graph size, it does not need re-measuring if `nTxEdgeBits` moves —
which is fortunate, since `PROOFSIZE` is explicitly out of scope.
