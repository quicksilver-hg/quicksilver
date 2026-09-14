# Mainnet Difficulty Floor and Coupled tx-PoW Cost

The launch difficulty floor is not only a block-time knob: `RequiredTxWork` is
mean block work over the MA window divided by
`nTxWorkCouplingK`, floored at 1 cycle. The floor therefore sets both how long a
block takes to find and what a transaction costs to send. This document records
the measurement, the chosen constants, and the reasoning.

Model script: `contrib/calibration/mainnet_floor_model.py`, pinned to the shipped
`powLimit` and to the four-card re-derivation by
`contrib/calibration/test_mainnet_floor_model.py`
(`python3 -m pytest contrib/calibration/test_mainnet_floor_model.py`).

## 1. Measured single-GPU throughput

Measured 2026-07-20 on a six-GPU Pascal reference rig, one GPU per search and
six searches in parallel. One candidate host was excluded after CUDA device
initialization failed; no measurements from that host are included.

The solver was rebuilt from the source tree rather than reusing an existing
binary:

```
make -C src/crypto/cuckatoo/gpu clean
make -C src/crypto/cuckatoo/gpu EDGEBITS=29 GPU_ARCH=sm_61
```

This is the shipped solver: `vendor/` contains only the **lean** implementation,
and `qsgpusolve.cu` is byte-identical to the rig's copy. The single E29 binary
serves both block PoW and per-tx PoW after the E29 unification.

Each sample runs `qsgpusolve 29 <preimage> <start_nonce> 100000` from a distinct
start nonce until it finds a 42-cycle, recording graphs consumed and wall time.
24 samples, 4 per GPU, on 6× P104-100 (the P106-100 was excluded as a different
card):

| quantity | value |
| --- | --- |
| samples | 24 (24 cycles found, 0 exhausted) |
| total graphs | 984 |
| total GPU-seconds | 3853.6 |
| graph rate | **3.916 s/graph** (0.2553 graphs/s) |
| P(42-cycle per graph) | **0.0244** (mean 41.0 graphs/cycle) |
| **cycle rate** | **0.006228 cycles/s per GPU** (160.6 s/cycle) |

The graph rate is the stable quantity (984 samples). The per-device cycle rates
range 0.0039–0.0117 cycles/s, which is the variance of a geometric process at
n=4 per device, not a hardware difference.

**Corrected 2026-08-07 (M7): 3.916 s/graph is contention-inflated by ~18%.** It was
taken with six GPUs searching in parallel on one host, timed as wall-clock across
whole `qsgpusolve` invocations including process startup. One idle P104-100 timed
with CUDA events gives **3.2126 s/graph** over 800 graphs (SD 0.55%), so the true
single-card E29 rate is **0.007274 cycles/s**, not 0.006228. Every block-time
figure derived below is correspondingly ~14% long. The correction is symmetric
across the E28 flag day, because both sides scale from this one anchor — see
§ "The floor after the E28 flag day" and
[`/tools/calibration/m7-same-card-ratio/README.md`](../../tools/calibration/m7-same-card-ratio/README.md).

Note also that this is a **solver-isolation** measurement, not an end-to-end node
measurement: each sample invokes `qsgpusolve` directly, as the recipe above shows.
Later text that described it as end-to-end was wrong; M7 measured the node path
separately and found no distinguishable per-attempt overhead.

**The remembered "~4 cycles/s per GPU" figure was wrong by ~300x** and is
retracted. Had it been reused, the launch floor would have been set ~300x too
high and one GPU would never have found block 1.

**Independent cross-check.** publictest runs `powLimit` ≈ 2^255 (2 cycles/block)
and a three-node cluster measured ~150 s mean block time, i.e. ~0.0133 cycles/s
aggregate, ~0.0044–0.0067 cycles/s per contributing GPU. That is the rate
measured here, arrived at from live chain behaviour rather than the solver.

## 2. Model output (at the E29 cycle rate)

Historical: computed at 0.006228 cycles/s, the E29 rate. The shipped size is E28
at 0.012840 cycles/s, which halves every block-time figure in this table — so the
chosen row is the 4-cycle one, at 311.5 s rather than the 642.3 s shown here. See
section 3 and `tools/calibration/e28-floor.md`. (M7 later corrected the anchor for
six-GPU contention: the shipped rate is 0.014935 cycles/s and the 4-cycle row is
**267.8 s**. The table is left at the original rate as the record of what was
derived at the time; see "Settled 2026-08-07" in section 3.)

`python3 contrib/calibration/mainnet_floor_model.py --cycles-per-second 0.006228 --ma-window 144`

| floor (cycles/block) | nBits | block time, 1 GPU | tx work | tx grind, 1 GPU |
| --- | --- | --- | --- | --- |
| 1 | `0x2100ffff` | 160.6 s | 1 cycle | 160.6 s |
| **2** | **`0x207fffff`** | **321.1 s** | **1 cycle** | **160.6 s** |
| 3 | `0x20555555` | 481.7 s | 1 cycle | 160.6 s |
| 4 | `0x203fffff` | 642.3 s | 1 cycle | 160.6 s |
| 8 | `0x201fffff` | 1284.5 s | 1 cycle | 160.6 s |
| 16 | `0x200fffff` | 2569.0 s | 1 cycle | 160.6 s |
| 106 | `0x20026a43` | 17019.9 s | 1 cycle | 160.6 s |

## 3. Chosen launch floor

> **Correction (2026-08-02, E28 flag day).** This is the second correction to this
> document in two days; the first is the inverted-`r` note in section 5. **Sections
> 1 and 2 above are the E29 measurement and remain accurate as a record of what was
> measured — but the floor they concluded with, 2 cycles / `0x207fffff` / 321 s, is
> no longer what ships.** Both Cuckatoo graph sizes moved 29 → 28, which does not
> change how many cycles a block costs but makes each cycle 2.06x cheaper. Holding
> `nBits` constant would therefore have halved the absolute work in the cheapest
> possible block. The floor is re-derived below at **4 cycles / `0x203fffff` /
> 311.5 s** — since remeasured at **267.8 s**, see "Settled 2026-08-07"; the
> `nBits` is unchanged. The arithmetic is in
> [`tools/calibration/e28-floor.md`](../../tools/calibration/e28-floor.md).
> Section 2's model table is computed at the E29 rate and is retained as the record
> of that derivation — its 4-cycle row reads 642.3 s because it assumes the E29
> cycle rate, not the shipped one.

**4 cycles per block.**

- `powLimit` = `0x3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff` (2^254 − 1)
- compact `nBits` = `0x203fffff`

`nPowTargetSpacing` is 300 s. The E28 cycle rate is the measured E29 rate scaled by
the M3 per-graph ratio, 0.006228 × (2.3907 / 1.1596) = **0.012840 cycles/s**. At
that rate a 4-cycle floor puts one GPU at **311.5 s per block** — the floor value
that lands on the target spacing. 2 cycles/block now gives 155.8 s (2x too fast);
8 cycles/block gives 623 s.

The floor is stated in *real work per block*, not in `nBits`. That distinction is
what makes this change necessary: at E28 the old 2-cycle floor would have produced
155.8 s blocks and made a minimum-difficulty chain twice as cheap to produce. The
new floor reproduces the original calibration to within 3% — 311.5 s against the
E29 derivation's 321.1 s — so the cheapest block costs the same real work after the
flag day as before it. The chain must be startable, and restartable, by a single
GPU; that requirement is unchanged and is what both floors encode. What "a single
GPU" means quantitatively is settled under "What 'one commodity GPU' means here,
exactly" below — the anchor is a P104-class card; clean commodity cards restart
the chain at 9–10 minute blocks, with a conservative degraded bound of 13.5
minutes, rather than at target.

`sandbox` is not part of this flag day: it stays at E19 and keeps `powLimit` =
2^255 − 1.

**Originally applied in the initial floor change (2026-07-21); re-derived for
the E28 flag day (2026-08-02).** Genesis `nBits` must equal the chain `powLimit` (guard:
`pow_tests/genesis_nbits_equals_chain_powlimit`), so the floor change and the
genesis re-mint land as one atomic commit — as they did initially, and again here.
Both `main` and `publictest` genesis hashes are re-minted, and every datadir for
those chains is invalidated. See `doc/audit/genesis-provenance.md`.

**Settled 2026-08-07 (M7), and the floor is unchanged.** The 0.012840 figure was
inferred rather than measured: it scaled the E29 anchor by a per-graph ratio taken
on a *different card* (the anchor on the rig's P104-100s, the ratio on a
P102-100). Measuring both sizes on one P104-100 gives **2.0531** against M3's
2.0617 — 0.42% apart across cards whose absolute speeds differ by 34%. The borrow
was sound and **the 2 → 4 cycle move was correctly sized**.

Two corrections fall out. First, the stated risk was misdirected: 0.006228 is a
solver-isolation measurement, not an end-to-end node one, and two mining nodes
measured over the later soak reach the isolated solver's rate to within the
±4.7% uncertainty on the cycle-rate constant — per-attempt overhead is not
distinguishable from zero. Second, the anchor is ~18% pessimistic from six-GPU
contention, which moves the floor block times *down*, not up:

| | published | measured | floor | block time |
| --- | ---: | ---: | ---: | ---: |
| E29 cycles/s | 0.006228 | 0.007274 | 2 | 321.1 s → **275.0 s** |
| E28 cycles/s | 0.012840 | 0.014935 | 4 | 311.5 s → **267.8 s** |

The equal-real-work property tightens to 2.6%. Both floors are ~11% fast of the
300 s target rather than ~4% slow, which is the safe direction for a bound whose
purpose is that one commodity GPU can start and restart the chain. The floor stays
at 4 cycles. Full record:
[`/tools/calibration/m7-same-card-ratio/README.md`](../../tools/calibration/m7-same-card-ratio/README.md).

**F-174 resolved 2026-09-08 from the final, condition-stated four-card inputs;
the floor is unchanged.** The 2026-09-05 table mixed calibration runs taken in
different launch and display conditions. It was conservative, but it was not a
single instrument and is superseded by the final-slot quiet medians below.
Reproduce with
`python3 contrib/calibration/mainnet_floor_model.py --fleet --floors 4`; the
arithmetic is pinned by `contrib/calibration/test_mainnet_floor_model.py`.

Each input is an E28 `qs-solver --check` result over SSH: the node stopped, no
solver process, display duty off the mining card, six graphs per sample, and the
median of three samples. Each card was already in its final launch slot. Linux
had no other compute client. `windowsqs1` was also clear; `windowsqs2` retained
seven idle desktop/UWP contexts, which is part of its stated condition. Full
measurement record: `qs-planning/plans/assets/2026-09-06-baselines.md`.

| card | host | quiet s/graph | cycles/s at pooled point yield | alone at floor |
| --- | --- | ---: | ---: | ---: |
| P104-100 | `linuxqs1` | 1.4667 | 0.0159337 | 251.0 s (0.84× target) |
| GTX 950 | `linuxqs2` | 3.4383 | 0.0067970 | 588.5 s (1.96×) |
| GTX 1050 Ti | `windowsqs1` | 3.1912 | 0.0073233 | 546.2 s (1.82×) |
| GTX 960 | `windowsqs2` | 3.2093 | 0.0072820 | 549.3 s (1.83×) |
| **all four** | | | **0.0373359** | **107.1 s (0.36×)** |

Graph throughput and cycle yield are separate measurements. The table converts
the quiet graph medians with M1+M2's pooled point estimate of **0.02337
cycles/graph** (451 cycles over 19,300 graphs). It does not substitute a live
node's quantized `attempts_per_second` or silently treat quiet capacity as a
production guarantee.

*The real opening law still selects four cycles.* The first retarget does not
observe 144 spacings: block 144 can see only the **143** spacings from genesis
through block 143. Later periods observe all 144 because F-147 anchors them on
the previous period's last block. Four cycles opens at 107.1 s; the first
retarget arrives after 4.26 h and requests **2.82×**, inside `src/pow.cpp`'s 4×
clamp. Two cycles opens at 53.6 s and requests **5.64×**, so it clamps and needs
a second epoch. Three cycles requests 3.76×, leaving only 6% headroom while
cutting cheapest-block work by 25%. Neither is preferable to the shipped floor.

*The clean and conservative survivor cases are both explicit.* At the point
estimate the GTX 950 is now the slowest card. If the full fleet establishes its
11.20-cycle steady work and then collapses immediately after a retarget, a clean
GTX 950 takes **2.75 days to reach the floor** and then remains at 588.5 s/block.
The conservative case applies the fleet's +25% graph-time health boundary to
the survivor and M1+M2's lower 95% cycle yield of **0.02124 cycles/graph** to
the whole model. It takes **3.43 days to reach the floor** and settles at
**809.4 s/block (13.5 min)**. That settled interval is persistent degraded mode,
not a transient that another retarget can remove.

This bound is deliberately a scenario, not a confidence interval for hardware
health. It is still inside section 4's accepted recovery envelope. Lowering the
floor would save several minutes in the lone-card case by weakening every
minimum-work block forever; raising it would worsen recovery without solving a
measured problem. **No change to the floor is warranted, and none is made.**

*Transaction cost is untouched.* `RequiredTxWork` is 1 cycle at the floor, at
four-card steady state, and in the conservative case. The floor stops governing
tx cost only above 0.3533 cycles/s; the quiet four-card point rate is 10.6% of
that threshold.

### What "one commodity GPU" means here, exactly

The floor's stated purpose above — that one commodity GPU can start and restart the
chain near the 300 s target — is **anchored on a P104-100**, which is a headless
mining card, not a commodity one. Measured by the standardized quiet probe, only
the anchor meets the target: the three commodity cards land at 1.8×–2.0× it,
i.e. 9–10 minute blocks for a clean lone survivor. The conservative slowest-card
case is 13.5 minutes.

This is deliberate and is not a target miss. The floor bounds *how easy a block may
ever become*, and pricing it for the slowest commodity card would require dropping
to ~1.67 cycles — which halves the real work in the cheapest possible block, the
exact property the E28 flag day existed to preserve, and which now also clamps the
opening retarget. The honest statement of the bound is therefore:

> The floor guarantees that a **single P104-class GPU** restarts the chain near
> target (251 s at the quiet point estimate), and that any clean commodity card
> in the launch fleet restarts it at **9–10 minute blocks**. The conservative
> degraded bound is **13.5 minutes**. Degraded-mode block time, not target-rate
> block time, is what the floor promises for commodity hardware.

### Interaction with the retarget arithmetic

A floor near 2^255 does **not** fit under the old "keep `powLimit` below
2^256/(4·`nPowTargetTimespan`)" ceiling — that would require
4·timespan < 2, which is unsatisfiable for any window. The ceiling was a proxy
for "the 4x retarget step must be representable", and the design has outgrown it.

`ScaleTarget` now handles the regime directly: it multiplies first
where safe, divides first above that, and **saturates** at the 256-bit maximum
where a 4x step has no 256-bit representation at all (which begins around
2^254). Both callers clamp the result to `powLimit`, which is exactly the value
saturation stands in for. The invariant that matters is therefore tested
directly — scaling up never decreases a target, and every calculated retarget is
accepted by `PermittedDifficultyTransition` — rather than via the old ceiling.

The wrap this replaced was not benign: a 4x step off 2^254+2^200 previously
returned a target ~2^56 times *smaller*, turning "blocks are too slow, ease off"
into a large difficulty increase. On a low-hashrate chain that is a death
spiral, and it would have been unfixable after launch.

## 4. Chosen retarget window

**144 blocks = 12 hours** (`nPowTargetTimespan = 144 * 5 * 60` = 43200 s).

Recovery from a 10x hashrate step, given the 4x-per-adjustment clamp (two
adjustments are needed: 4x then 2.5x):

| window | 10x hashrate arrives | 10x hashrate leaves |
| --- | --- | --- |
| **144 blocks (12 h)** | **6.0 h** | **6.25 days** |
| 2016 blocks (7 d) | 3.5 days | 87.5 days |

The downward direction is the one that matters. If the founder's hashrate leaves
a 2016-block chain, difficulty stays ~10x too high for **nearly three months** —
long enough that a newcomer cannot restart the chain in any practical sense. At
144 blocks the same event corrects in about six days. The upward direction is
the mirror risk: a newcomer arriving with better hardware mines at a stale, easy
difficulty for 3.5 days at 2016 versus 6 hours at 144.

144 blocks is one fourteenth of the inherited 2016-block window, while keeping
the inherited retarget arithmetic and its 4x clamp unchanged. Shorter windows
correct faster but sample fewer blocks per adjustment, so they track noise rather
than hashrate; 12 hours is short enough to make the chain restartable and long
enough that a single lucky streak does not move the target.

## 5. The opening window, quantified

`RequiredTxWork` is mean block work over `nBaseWorkMAWindow`, divided by `K`,
scaled by the congestion multiplier, floored at 1 cycle. At the launch floor:

```
block work = 4 cycles  ->  4 / 106 = 0  ->  clamped to the floor of 1 cycle
```

(At E29 this read `2 / 106`; integer division floors to 0 either way, so the
conclusion is unchanged.)

So from height 0 the per-transaction cost sits at its floor of **1 cycle**. The
important finding is that **this floor is not cheap**: one cycle is 77.9 s of
grinding on one GPU at E28 — down from 160.6 s at E29, because a cycle is now
2.06x faster to find. There is still no window in which transactions are nearly
free.

How long the floor holds: `RequiredTxWork` only exceeds 1 cycle once mean block
work exceeds `K` = 106 cycles, i.e. once network hashrate exceeds
106 / 300 s = 0.353 cycles/s. **At E28 that is ≈ 28 GPUs, down from ≈ 57 at E29** —
the threshold halves because each GPU now contributes twice the cycle rate. Below
that the floor governs
regardless of height — this is a hashrate threshold, not a ramp that expires.

Attacker throughput at the floor:

| attacker | tx/hour | tx/s |
| --- | --- | --- |
| 1 GPU | 46.2 | 0.0128 |
| 7-GPU rig | 324 | 0.090 |
| 28 GPUs | 1294 | 0.359 |

Every row doubles at E28, and the third row is now 28 GPUs rather than 57, because
that is where the floor stops binding.

Block capacity is `N_tx,max` = 2·COIN / `nTxPowMint` = 2·10^8 / 57143 ≈ 3500
transactions per block, i.e. ~11.7 tx/s at 300 s spacing. A single-GPU attacker
sustains 0.11% of capacity; even 28 GPUs — the point at which the floor stops
binding — reach 3%. **The conclusion is unchanged by the flag day**: attacker
throughput and the binding threshold both doubled, so their ratio to capacity did
not move. The floor of 1 cycle is a real cost, and the opening window is not a
spam exposure.

## 6. Decision on `nBaseWorkMAWindow`

**Follows the retarget window to 144 blocks** (from 2016).

The existing comment describes it as "aligned to difficulty-retarget interval",
and that alignment is the substantive property: transaction cost tracks the same
difficulty epoch the block target does. Leaving it at 2016 while the retarget
window moves to 144 would make the stated rationale false and let tx cost lag
block difficulty by up to a week.

A longer MA window than the retarget interval also lengthens the period in which
tx cost reflects stale, lower difficulty. That is inert at launch — the floor of
1 cycle governs until ~57 GPUs either way — but it is the wrong direction, and
there is no reason to bank it.

The counter-argument is that 2016 gives seven days of smoothing and is harder to
game via short-term difficulty manipulation. At 144 blocks the smoothing is 12
hours, which is still 144 independent samples; the congestion multiplier, not
the MA window, is the fast-moving term in tx cost.

## 7. Re-derivation of `K` = 106

`K` = α · S_tail / (C · r), where α = 0.25 (mint-safety margin), r is the tx-solver
rate relative to the block-solver rate, S_tail = 1 COIN = 10^8 cinnabar, and
C = `nTxPowMint` = 57143 cinnabar minted per transaction.

> **Correction (2026-08-02, Stage 1 finding).** This ceiling previously placed `r`
> in the *numerator*, which inverts it. Spend one second on each path: the
> transaction path completes `K / (t_tx · W)` transactions, each minting `C`; the
> mining path completes `1 / (t_block · W)` blocks, each minting `S_tail`. The
> ratio is `C · K · r / S_tail`, so a **faster** transaction solver makes minting
> by transaction more attractive and must be offset by a **smaller** `K` — more
> work per transaction — not a larger one. Reductio: per-tx PoW at E19 against
> block PoW at E29 gives `r` = 703, and the numerator form permits `K` = 307,626,
> which drives `RequiredTxWork` to ~0 and mints for free. **Nothing shipped is
> affected**, because both forms agree at `r` = 1, which unifying the two graph
> sizes guarantees — at E29 when this was written, and at E28 after the flag day.
> Derived and tested in `tools/calibration/model.py`
> (`max_k_for_mint_safety`, `mint_safety_ratio`) and
> `tools/calibration/test_model.py`.

| basis | r | derived K |
| --- | --- | --- |
| E31 lean (original calibration) | 0.243 | 1800.4 |
| E31 mean, run3 on L40S | 0.363 | 1205.2 |
| **unified, E28 (current — was E29)** | **1.0** | **437.5** |

The first two rows are restated under the corrected form above, computed by
`tools/calibration/model.py::max_k_for_mint_safety`; the historical
values 106.3 and 158.8 came from the inverted one. Only the unified row, where
`r` = 1 and the two forms coincide, was ever load-bearing.

Unification made block PoW and per-tx PoW the *same graph size and the same
solver*, so work is measured in identical cycle units on both sides and r = 1 by
construction. The derived ceiling therefore rises to 437.

**This row is why the E28 flag day cost no mint-safety margin.** `r` is a *ratio*
of two solver rates. Moving both sizes together leaves it at 1.00 exactly — the
numerator and denominator scale by the same 2.06x — so this table, the derived
`K` = 437.5 ceiling, and the 4.1x margin below all carry over from E29 to E28
untouched. Moving only `nTxEdgeBits` would have driven `r` to ~2 and halved the
margin for the same user-facing speedup, which is precisely the trade the unified
move avoids.

**Verdict: `K` = 106 still holds, with 4.1x more margin than the design
requires.** `K` divides block work, so a smaller `K` demands *more* work per
transaction — the conservative direction. The safety property is that minting
via transactions must not out-earn mining per unit work:

```
value per unit work, tx vs block = C * K / S_tail
  K = 106  ->  0.0606   (16.5x margin; requirement is 4x)
  K = 437  ->  0.2497   (4.0x margin; exactly the alpha = 0.25 limit)
```

No change to `K` was made. Retuning to 437 would make transactions 4x cheaper in
work terms, but the change is inert at launch (the 1-cycle floor binds until ~57
GPUs with K=106, ~235 GPUs with K=437) and it would spend the entire margin. The
prior decision to hold `K` at 106 rather than retune stands, now for a stronger
reason than when it was made: the constraint moved in the permissive direction
and the chain does not need the room.
