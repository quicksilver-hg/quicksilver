# M7 — the E29-versus-E28 ratio on one card

- Date: 2026-08-07
- Git commit: `b7277d2`
- GPU: NVIDIA P104-100, 8 GB, compute capability 6.1, driver 580.173.02
- `GPU_ARCH`: `sm_61`; CUDA 12.0.140
- Harness: `crypto/cuckatoo/gpu/qsgpucalibrate.cu`, built once per graph size
- Raw data: `p104-e29-a.csv`, `p104-e28.csv`, `p104-e29-b.csv`; host detail in `meta.txt`

Task 5.2 of the launch-readiness program. This closes the open item carried by
both [`../e28-floor.md`](../e28-floor.md) and
[`/doc/audit/mainnet-difficulty-floor-model.md`](../../../doc/audit/mainnet-difficulty-floor-model.md).

## What was actually owed

The E28 difficulty floor is derived by scaling a measured E29 cycle rate by the
E29/E28 per-graph ratio:

```
rate28 = 0.006228 * (2.3907 / 1.1596) = 0.012840 cycles/s
```

The two inputs come from **different cards**. `0.006228` cycles/s (3.916 s/graph)
was measured on the rig's **P104-100** cards; the `2.3907 / 1.1596` ratio is M3,
measured on the calibration **P102-100**. The P102 solves an E29 graph 1.64× faster than
the P104, so the cards are demonstrably not interchangeable in absolute terms.
Whether their *ratio* agrees was never checked, and the floor rests on it.

M3 had already measured one card at both sizes, so the plan's framing — "the same
card at both graph sizes is still owed" — was satisfied for the P102. What was
genuinely unconfirmed is that the ratio **transfers** to the card the anchor was
taken on.

## Method

Both binaries are compiled from the same source at the same `sm_61`, differing
only in `-DEDGEBITS`. `qsgpucalibrate` sweeps nonces `0 … graphs-1` from a fixed
pre-image, so **the two sizes search the identical nonce sequence** and the
comparison is paired. Timing is CUDA events around `run_solver`, after one untimed
warm-up graph on an out-of-range nonce.

The card was idle: the node's miner was stopped (`stopmining`) and `qsgpusolve`
confirmed absent before the first run, and re-armed afterwards. Runs are ordered
E29 → E28 → E29 so thermal drift cannot be mistaken for a ratio.

## Results

| run | graphs | mean s | median s | SD | SD % of mean | cycles found |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| E29-a | 600 | 3.2118 | 3.2105 | 0.0176 | 0.55% | 10 |
| E28 | 600 | 1.5648 | 1.5641 | 0.0109 | 0.70% | 11 |
| E29-b (drift check) | 200 | 3.2152 | 3.2144 | 0.0177 | 0.55% | 4 |
| **E29 pooled** | **800** | **3.2126** | — | 0.0177 | 0.55% | 14 |

Drift between the two E29 blocks, an hour apart and either side of the E28 run:
**+0.11%**. Negligible, so the ordering did not bias the ratio.

```
ratio = 3.2126 / 1.5648 = 2.0531    95% CI [2.0517, 2.0545]
```

## Verdict — the ratio transfers

| card | E29 s/graph | E28 s/graph | ratio |
| --- | ---: | ---: | ---: |
| P102-100 (M3 calibration) | 2.3907 | 1.1596 | 2.0617 |
| P104-100 (M7 calibration) | 3.2126 | 1.5648 | **2.0531** |

The two differ by **0.42%**, across cards whose absolute speeds differ by 34%.
The cross-card borrow in the floor derivation is sound, and **the 2 → 4 cycle
floor move was correctly sized**. Task 5.2's stop condition — "if the ratio
contradicts the 2→4 cycle floor move, that is a consensus finding and stops this
phase" — is **not** met. No consensus change is indicated.

`test_model.py` asserts this ratio as 2.062; M7 confirms it and the assertion
stands unchanged.

## Secondary finding — the absolute anchor is ~18% pessimistic

The ratio is confirmed, but the *absolute* rate it is applied to is not. The rig
anchor of 3.916 s/graph was taken with **six GPUs running in parallel on one
host**, timed as wall-clock across whole `qsgpusolve` invocations including
process startup. One idle card, timed with CUDA events, gives **3.2126 s/graph** —
18.0% faster.

Applying the M1+M2 pooled cycle rate of 0.02337 cycles/graph (42.8 graphs/cycle):

| | published | M7 measured | floor | block time |
| --- | ---: | ---: | ---: | ---: |
| E29 cycles/s | 0.006228 | **0.007274** | 2 | 321.1 s → **275.0 s** |
| E28 cycles/s | 0.012840 | **0.014935** | 4 | 311.5 s → **267.8 s** |

The equal-real-work property the floor exists to hold is **unaffected**: 275.0 s
against 267.8 s is 2.6% apart, tighter than the 3% the flag day claimed. Both
floors are simply ~11% fast of the 300 s target rather than ~4% slow, and the
error is symmetric across the flag day because it lives in the shared anchor.

Fast is the safe direction — the floor exists so one commodity GPU can start and
restart the chain, and a cheapest block that arrives at 268 s rather than 311 s
serves that purpose at least as well. **Recommendation: leave the floor at 4
cycles.** Re-minting genesis a third time to recover 11% on a bootstrap-only
bound is not worth the cost.

## The deferred end-to-end confirmation, now closed

`e28-floor.md` deferred one further obligation: `0.012840` had never been seen on
a *mining node*, only on the solver in isolation, and "end-to-end rate includes
per-attempt overhead that need not scale with graph size."

Two nodes on the same card model, over the Phase 5 soak:

| host | card | elapsed | cycles found | s/cycle |
| --- | --- | ---: | ---: | ---: |
| P104 node A | P104-100 | 34.3 h | 1963 | 63.0 |
| P104 node B | P104-100 | 38.9 h | 2290 | 61.2 |
| M7 solver in isolation | P104-100 | — | — | 67.0 |

The node reaches the isolated solver's rate; per-attempt overhead is not
distinguishable from zero and is bounded below the ±4.7% Poisson uncertainty on
the 42.8 graphs/cycle constant itself. A 4096-graph window amortises template
construction over roughly 67 s of solving, which is why. **The concern the open
item raised does not materialise**, and the two documents' framing of `0.006228`
as an "end-to-end node measurement" was wrong in the first place — §1 of the floor
model shows it came from invoking `qsgpusolve` directly, with no node in the loop.

Live corroboration: the P102 node measured 26 floor-difficulty intervals at a mean of
255.3 s during the E28 flag-day gate, against 252 s predicted from its own
node-level rate.

## Reading the node figures — the counter this table was built from is gone

**Historical.** The two node rows above were backed out of `getminingstatus` as it
behaved when M7 ran, and that arithmetic no longer applies. It is kept because the
table was derived with it.

At the time, `MiningStatus::attempts` was documented as "nonces swept this session"
and was not that either. `SolveBlockPoW` is called with `max_tries = 4096` and
decrements it by the **full window width** whether the solver swept 4096 graphs or
stopped at the first cycle. A losing window therefore always booked exactly 4096.
A *winning* window booked **zero**, because `SolveBlockPoW` returns before the
decrement. So:

```
attempts = 4096 * (cycles_found - blocks_found)      # M7-era only
cycles_found = attempts / 4096 + blocks_found        # M7-era only
```

which is what the table uses. The counter was a cycle counter scaled by 4096, not
a graph counter — it overstated real graph throughput by roughly 42×.

That is also the mechanism behind the dead-GPU misreport: a card that had fallen
off the bus failed instantly, every failure still booked 4096, and the node
reported hundreds of thousands of "attempts per second" while doing no work at
all. It is recorded here because M7 is where the counter's true meaning was
established.

**What the fields mean now.** `72bc627` re-based the counter on the solver's
per-graph `progress` callback, so `graphs_attempted` is a plain count of graphs —
divide nothing by 4096. `attempts_per_second` is a separate reading again: graphs
per second over the trailing 120 s, not a session average, and it is **omitted
from the response entirely** while no attempt has been observed. A future
repeat of this measurement should take `graphs_attempted` directly and pair it
with `elapsed_seconds`; the rate field answers "is the solver working now", not
"how much has it done".
