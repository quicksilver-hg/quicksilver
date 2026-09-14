# The difficulty floor at E28

Derivation of the `main` / `publictest` difficulty floor after both Cuckatoo graph
sizes move from 29 to 28. Companion to
[`doc/audit/mainnet-difficulty-floor-model.md`](../../doc/audit/mainnet-difficulty-floor-model.md);
this file is the arithmetic, that file is the model.

## What the floor is for

The floor is the *easiest block the chain will ever accept*. Its purpose is
bootstrap and recovery: **one GPU must be able to start the chain, and restart
it**, producing blocks near the 300 s target when it is the only miner on the
network.

> ⚠ **The anchor is a P104-100, a headless mining card — not a commodity one.**
> Resolved by F-174 against the final-slot quiet medians: only the P104 meets
> "near target" (251 s). Clean commodity cards land at 546–588 s (9–10
> minutes), with a conservative degraded bound of 809.4 s (13.5 minutes).
> That is the floor's real promise for commodity hardware and it is deliberate;
> see the "What 'one commodity GPU' means here,
> exactly" section of
> [`doc/audit/mainnet-difficulty-floor-model.md`](../../doc/audit/mainnet-difficulty-floor-model.md).

It is not a difficulty setting — retargeting moves difficulty up from
here as hashrate arrives. It is the lower bound retargeting may never cross.

That purpose is stated in units of *real work per block*, not in units of nBits.
This is the whole reason the floor has to move.

## Why holding nBits constant would be wrong

`nBits` encodes a *target*, and a target is a statement about how many Cuckatoo
cycles a block costs — not about how long they take to find. Moving E29 → E28 does
not change how many cycles the floor demands; it changes how fast a GPU produces
each one.

So holding `nBits` at `0x207fffff` across the flag day would keep the floor at
2 cycles while making each cycle 2.06× cheaper. The absolute work in the cheapest
possible block would halve. Concretely, the minimum-difficulty block would fall
from 321 s to **155.8 s** — roughly half the 300 s target — and a
minimum-difficulty chain would become twice as cheap to produce.

Holding the *principle* constant, rather than the constant, is what preserves the
design. The floor moves 2 → 4 cycles so that the cheapest block costs the same
real work after the change as before it.

## The measurement

| | E29 | E28 | ratio |
|---|---|---|---|
| GPU seconds per graph (M3, P102-100) | 2.3907 | 1.1596 | 2.062× |
| Cycles/s, one commodity GPU (P104-100 rig) | 0.006228 | 0.012840 | 2.062× |

```
rate28 = 0.006228 * (2.3907 / 1.1596) = 0.012840 cycles/s
```

The two rows come from **different cards** — the rate from the rig's P104-100s, the
ratio from the P102-100 calibration host — so this scaling borrows a ratio across hardware. M7
confirmed the borrow on 2026-08-07 by measuring both sizes on one P104-100:
**2.0531**, 95% CI [2.0517, 2.0545], which agrees with M3's 2.0617 to 0.42% across
cards whose absolute speeds differ by 34%. See
[`m7-same-card-ratio/README.md`](m7-same-card-ratio/README.md).

## The floor

| floor | seconds/block at E28 | vs 300 s target |
|---|---|---|
| 2 cycles | 155.8 s | ~half target — too cheap |
| **4 cycles** | **311.5 s** | **+3.8%, reproduces the original calibration** |

For reference, the E29 derivation this replaces: 2 / 0.006228 = **321.1 s**.

4 cycles at E28 (311.5 s) lands within 3% of 2 cycles at E29 (321.1 s). The cheapest
block costs the same real work after this change as before it, which is the property
the floor exists to hold.

## From cycles to powLimit and nBits

`GetBlockProof` is `2^256 / (target + 1)`, so a floor of *n* cycles is the target
that makes that quotient *n*:

```
work = 4  =>  target + 1 = 2^256 / 4 = 2^254
          =>  target = 2^254 - 1
          =>  powLimit = 0x3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
          =>  compact nBits = 0x203fffff
```

Cross-check against the value being replaced, which the same arithmetic must
reproduce:

```
work = 2  =>  target = 2^255 - 1 = 0x7fff...ff  =>  nBits = 0x207fffff   (current E29 value)
```

Genesis `nBits` and the chain's `powLimit` must be the same number; the
`genesis_nbits_equals_chain_powlimit` unit test is the guard that a half-applied
change cannot pass.

`sandbox` is not part of this flag day. It stays at E19 and keeps
`powLimit = 2^255 - 1`.

## Closed 2026-08-07 — the scaling was inferred, and is now measured

This section previously recorded the scaling as deferred-not-optional. M7 measured
it. Full record in [`m7-same-card-ratio/README.md`](m7-same-card-ratio/README.md);
the two things it changes here are below.

**The ratio holds, so the floor move was correctly sized.** Measured on one
P104-100 at both sizes: **2.0531** against M3's 2.0617 on a P102-100. The 2 → 4
cycle move stands and no consensus change is indicated.

**The absolute anchor was ~18% pessimistic, symmetrically on both sides.** The
worry stated here — that per-attempt node overhead would make the true E28 rate
*lower* than the scaling implies — does not materialise, and was misdirected: it
described `0.006228` as an end-to-end measurement on a mining node, when §1 of
`doc/audit/mainnet-difficulty-floor-model.md` shows it came from invoking
`qsgpusolve` directly. Two mining nodes measured over the Phase 5 soak reach the
isolated solver's rate to within the ±4.7% uncertainty on the cycle-rate constant,
so overhead is not distinguishable from zero.

What the anchor *did* carry is contention: 3.916 s/graph was taken with six GPUs
running in parallel on one host. One idle card gives 3.2126 s/graph. Corrected:

| | published | measured | floor | block time |
|---|---:|---:|---:|---:|
| E29 cycles/s | 0.006228 | 0.007274 | 2 | 321.1 s → **275.0 s** |
| E28 cycles/s | 0.012840 | 0.014935 | 4 | 311.5 s → **267.8 s** |

The equal-real-work property tightens rather than breaks — 267.8 s against 275.0 s
is 2.6% apart, where the figures above claim 3%. Both floors are ~11% fast of the
300 s target instead of ~4% slow, which is the safe direction for a bound that
exists so one GPU can start and restart the chain. **The floor stays at
4 cycles**; re-minting genesis to recover 11% on a bootstrap-only bound is not
worth the cost.

**F-174 resolved 2026-09-08 from four final-slot quiet medians.** P104-100
1.4667, GTX 950 3.4383, GTX 1050 Ti 3.1912, and GTX 960 3.2093 s/graph aggregate
to 0.0373359 cycles/s at the pooled point yield. The 4-cycle floor opens at
107.1 s. The first retarget observes 143 spacings, not 144, and requests 2.82× —
inside the 4× clamp. At 2 cycles it requests 5.64× and clamps; at 3 cycles it
requests 3.76× while discarding 25% of the cheapest-block work. The floor stays
at 4 cycles.

The inputs are standardized quiet solver-capacity measurements, not a promise
of live-node throughput. A conservative survivor uses +25% graph time and the
pooled cycle experiment's lower 95% yield (0.02124 cycles/graph): the slowest
card, the GTX 950, settles at 809.4 s/block and takes 3.43 days to reach the
floor after a worst-aligned full-fleet collapse. The full derivation and exact
measurement conditions are in `doc/audit/mainnet-difficulty-floor-model.md`.
