# M3 — GPU time per graph, edge bits 22–29 (plus 19)

- Date: 2026-08-01
- Git commit: `d4a9b38`
- GPU: NVIDIA P102-100, 10 GB, compute capability 6.1, driver 580.142
- `GPU_ARCH`: `sm_61`; CUDA 12.0.140
- Source data: `../m2-cycle-rate/gpu-e*.csv` (the same run supplies M2 and M3)
- Per-size summary: `summary.txt`

Timings are taken with CUDA events around `run_solver` for each graph
individually. One untimed warm-up graph runs before the first recorded row, on a
nonce outside the recorded range, so that module load and allocation do not land
inside the measured interval — without it the E22 mean, where a graph is
milliseconds, would be dominated by setup.

## Results

| Edge bits | Graphs | Mean s | Median s | SD | ×prev | CPU mean s (M1, 8 threads) | GPU speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 19 | 1300 | 0.0034 | 0.0028 | 0.0020 | — | — | — |
| 22 | 1300 | 0.0106 | 0.0099 | 0.0049 | 3.138 | 0.2090 | 19.7× |
| 23 | 1300 | 0.0197 | 0.0190 | 0.0067 | 1.858 | 0.3732 | 18.9× |
| 24 | 1300 | 0.0381 | 0.0375 | 0.0086 | 1.934 | 0.7387 | 19.4× |
| 25 | 1300 | 0.0995 | 0.0990 | 0.0104 | 2.610 | 1.4596 | 14.7× |
| 26 | 1300 | 0.2453 | 0.2450 | 0.0090 | 2.465 | 3.2192 | 13.1× |
| 27 | 1300 | 0.5472 | 0.5471 | 0.0036 | 2.231 | 9.0369 | 16.5× |
| 28 | 1300 | 1.1596 | 1.1592 | 0.0058 | 2.119 | 22.1299 | 19.1× |
| 29 | 1300 | 2.3907 | 2.3900 | 0.0102 | 2.062 | 49.6925 | 20.8× |

Distributions above E26 are extremely tight (SD under 0.5% of the mean); the
larger relative spread at E19–E24 is launch-overhead jitter on graphs that take
only milliseconds.

## The E29 anchor supersedes the 3.4 s code comment

The comment the plan expected to confirm records **3.4 s** per graph at E29 on
GPU, never verified. Measured here: **2.3907 s**, SD 0.0102 over 1300 graphs.

They do **not** agree. The GPU is 30% faster than the comment claims. The
comment should be replaced with the measured value and a pointer to this file,
as a Stage 2 documentation rider. The direction matters: the unverified figure
overstated the honest sender's cost, so nothing was under-provisioned by it.

## Speedup is not a constant, so CPU timings cannot be scaled to GPU

The GPU advantage ranges from **13.1× to 20.8×** and is not monotonic — it falls
through E25–E26 and recovers by E28. Any model that converts CPU cost to GPU cost
with a single factor is wrong by up to 60% depending on the size chosen.

The dip has a plausible mechanism, though this run does not isolate it: the two
processors have cache boundaries at different graph sizes. Each solver instance
holds two bitmaps totalling 2^EDGEBITS/4 bytes. On the CPU that crosses the
10 MiB L3 between E25 (8 MB) and E26 (16 MB), which is where M1 records its own
knee. The GP102's L2 is far smaller, so the GPU leaves cache several sizes
earlier and pays its penalty first; the speedup dips over the window where the
GPU has already spilled and the CPU has not, then recovers once both are in DRAM.

For the flag day, the practical consequence stands regardless of mechanism: use
the measured GPU row for the candidate size, never a scaled CPU row.

## Honest sender cost (criterion 5 input)

Grind time per transaction is per-graph time × 42.8 graphs per cycle (the
combined M1+M2 rate, `../m2-cycle-rate/README.md`):

| Edge bits | GPU grind | CPU grind (8 threads) |
| --- | ---: | ---: |
| 22 | 0.5 s | 8.9 s |
| 23 | 0.8 s | 16.0 s |
| 24 | 1.6 s | 31.6 s |
| 25 | 4.3 s | 62.5 s |
| 26 | 10.5 s | 2.3 min |
| 27 | 23.4 s | 6.4 min |
| 28 | 49.6 s | 15.8 min |
| 29 | **1.70 min** | **35.4 min** |

At the shipped E29, an honest sender with a $65 mining card waits **1.7 minutes**
per transaction; one with only the 8-thread reference desktop waits **35 minutes**.
That 21× gap is the substance of criterion 5, and it is a statement about who can
transact without delegating, not a number to optimise against the other four
criteria.

(M1's README quotes 36.2 min for the same E29 CPU figure. The difference is only
which cycle rate is applied — M1 used its own 0.02289, this table uses the
combined 0.02337. The combined estimate is the better one.)
