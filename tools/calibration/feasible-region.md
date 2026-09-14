# The feasible region

Every number here is measured (M1–M6) or derived from measurements by
`model.py`, which is unit-tested in `test_model.py`. Criteria are quoted from §6
of the spec as committed; none were moved during Stage 1.

**The region is not empty.** One combination satisfies all five criteria, and it
is not the one the spec's levers table would suggest.

---

## A correction that governs everything below

`doc/audit/mainnet-difficulty-floor-model.md:196` derives the mint-safety ceiling
as

    K = α · r · S_tail / C

with `r` the transaction-solver rate relative to the block-solver rate. **`r` is
on the wrong side.** The correct relation is

    K ≤ α · S_tail / (C · r)

Derivation. Spend one second on each path. The transaction path completes
`K / (t_tx · W)` transactions, each minting `C`; the mining path completes
`1 / (t_block · W)` blocks, each minting `S_tail`. The ratio of value per unit
real work is `C · K · t_block / (S_tail · t_tx) = C · K · r / S_tail`, which must
stay at or below α.

The reductio that decides it, using measured timings. Put per-transaction PoW at
E19 (0.0034 s/graph) against block PoW at E29 (2.3907 s/graph): `r` = 703, and
minting by transaction is ~700× cheaper per unit real work than mining. The
committed formula responds by *raising* the permitted `K` to 307,626 — that is,
by requiring `W/K ≈ 0` work per transaction. Free minting, which is the exact
failure α exists to prevent. The corrected form gives `K < 1`: one transaction
must cost more than one block, which is what compensating for a 700× cheaper
cycle actually requires.

**Nothing shipped is affected.** The two forms agree exactly at `r` = 1, and the
E29 unification makes `r` = 1 by construction, so `K` = 106 and its 4.1× margin
stand as recorded. The error becomes load-bearing only when `nTxEdgeBits` moves —
which is the spec's central lever. Correcting
`doc/audit/mainnet-difficulty-floor-model.md` is a Stage 2 rider.

---

## Criterion 3 eliminates six of the eight candidate sizes

`r` is measured directly: per-graph GPU time at the candidate size against
per-graph GPU time at E29, which remains the block-PoW size. With `K` held at its
shipped 106:

| nTxEdgeBits | GPU s/graph | r | K ceiling | mint-safety ratio | margin vs α | verdict |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 22 | 0.0106 | 225.3 | 1.94 | 13.6454 | 0.02× | FAIL |
| 23 | 0.0197 | 121.3 | 3.61 | 7.3458 | 0.03× | FAIL |
| 24 | 0.0381 | 62.7 | 6.98 | 3.7983 | 0.07× | FAIL |
| 25 | 0.0995 | 24.0 | 18.21 | 1.4552 | 0.17× | FAIL |
| 26 | 0.2453 | 9.75 | 44.89 | 0.5903 | 0.42× | FAIL |
| 27 | 0.5472 | 4.37 | 100.14 | 0.2646 | 0.94× | FAIL |
| 28 | 1.1596 | 2.06 | 212.21 | 0.1249 | 2.00× | PASS |
| 29 | 2.3907 | 1.00 | 437.50 | 0.0606 | 4.13× | PASS |

**Lowering `nTxEdgeBits` is self-defeating.** It is the only lever that makes
honest sending cheaper, and it cheapens minting-by-transaction by exactly the
same factor. E27 misses by a hair — mint-safety ratio 0.2646 against the α = 0.25
limit — and everything below fails by orders of magnitude. `K` = 106 tolerates a
transaction solver at most **4.13×** faster than the block solver; E27 is 4.37×.

This is the quantified form of the coupling §4 of the spec warns about, and it
settles the lever: unification is not merely convenient, it is close to forced.
Only E28 survives alongside a fixed E29, and at half the margin.

> **Frame note (2026-08-02, E28 flag day).** Every row above measures a candidate
> `nTxEdgeBits` **against a block-PoW size held fixed at E29** — that is stated in
> the opening sentence and it is the frame Stage 1 evaluated in. The table is
> correct for what it measured and is kept unchanged.
>
> **It does not cover the case that shipped.** Stage 3 moved `nEdgeBits` *and*
> `nTxEdgeBits` to 28 together. `r` is a ratio of two solver rates, so when both
> move by the same factor it stays **1.00 exactly** — the unified E28 case has the
> same `r`, the same `K` ceiling of 437.50, and the same 4.13× margin as the E29
> row, not the 2.06× / 2.00× shown in the E28 row here. That row is E28
> transactions against E29 blocks, which is not what ships.
>
> So the conclusion above needs one qualifier: lowering `nTxEdgeBits` *alone* is
> self-defeating. Lowering both together is free, and halves the honest sender's
> cost without spending any margin. See
> [e28-floor.md](e28-floor.md) and `doc/audit/mainnet-difficulty-floor-model.md`.

---

## Criterion 1 needs a size term, and fixes its slope

Without a size term the ratio is `n_max / K`, which at a 4 MB cap is 10/106 =
0.094 — the spec's finding 3.3, reproduced by the model rather than by prose.
Shrinking the block cap makes it *worse* (5/106 at 2 MB), because `n_max` falls
while `K` does not.

With a size term charging work per byte against a consensus-fixed reference
weight `R`, the attacker's slicing choice disappears and the ratio becomes
`(B/R) / (K·r)`. The asymptotic parity point is `R = B/(K·r)` — **and it is the
wrong answer.**

Criterion 1 is evaluated across the whole range of mean block work, 2 to 100,000
cycles, and `RequiredTxWork` floors `mean_block_work // K`. The binding case is
`W = 2K − 1 = 211`: the quotient is 1 while `W` is nearly `2K`, so almost half
the intended work is lost to the floor. At the asymptotic `R` the ratio dips to
0.9996 there. The true bound is

    R ≤ B / ((2K − 1) · r)

which is **about half** the asymptote. This is finding 3.2's integer division
reappearing in a place the spec did not track it — 3.2 notes that the floor makes
the congestion multiplier inert at launch, but the same floor also sets the size
term's slope, permanently.

| nTxEdgeBits | r | R at 4 MB cap | R at 2 MB cap | R at 1 MB cap |
| --- | ---: | ---: | ---: | ---: |
| 29 | 1.000 | 18,957 wt (4,739 vB) | **9,478 wt (2,369 vB)** | 4,739 wt (1,184 vB) |
| 28 | 2.062 | 9,193 wt (2,298 vB) | 4,596 wt (1,149 vB) | 2,298 wt (574 vB) |

Verified by scanning every integer `W` from 2 to 100,000: at the bound there are
**zero** violations; at the asymptotic value there are violations.
`model.max_reference_weight` computes it by scanning rather than by the closed
form, so a later change to `RequiredTxWork` cannot silently invalidate it.

**Round `R` down**, for the same reason at a smaller scale: exact parity at 4 MB
and E29 asymptotically is 37,735.85, and the nearest integer 37,736 already
fails at 0.999996. `test_model.py` guards both.

A size term is **necessary**, not merely helpful — no candidate passes criterion 1
without one. There is one alternative with the same effect: capping
`MAX_STANDARD_TX_WEIGHT` at `R` instead. That is the same arithmetic reached by
forbidding large transactions rather than pricing them, and it forecloses large
transactions entirely. The size term is strictly more permissive for the same
defence.

Honest senders are largely untouched. At the retained 4 MB cap and E29,
`R` = 18,957: a real two-output transaction measures 1,291 weight (M5), so it is
charged 0.068 of base work and the one-cycle floor governs until mean block work
reaches ~1,557 cycles, roughly 830 GPUs. Above that a normal transaction costs
about a fifteenth of base work, while an attacker's 400,000-weight transaction
costs 21× base — and filling a block costs at least what mining it costs.

(1,291 weight is the measured shape of an ordinary payment. M4's 1,119 is the
lightest transaction the harness can construct at all; it is the right number for
criterion 4's *maximum* mint and the wrong one for typical cost.)

---

## Criterion 2 is about traffic shape, not block size

**Superseded 2026-08-01.** The first pass treated M5's 4,055,004 bytes/block as
*the* cost of a full block and concluded the 4 MB cap had to shrink. That figure
belongs to one shape — 2,300 outputs per transaction. Two further shapes were
measured at the same weight cap:

| Shape | tx/block | tx/s @300s | Total bytes/block | GB/year |
| --- | ---: | ---: | ---: | ---: |
| Honest, 2 outputs | 3,099 | **10.33** | 1,445,751 | **152.0 PASS** |
| Mid, 200 outputs | 114 | 0.38 | 3,922,349 | 412.2 FAIL |
| Adversarial, 2,300 outputs | 11 | 0.04 | 4,055,004 | 426.3 FAIL |

**Honest traffic at the current 4 MB cap and 300 s spacing costs 152 GB/year** —
24% inside the ceiling, at full throughput. Nothing about capacity needs to move.

This matters because **throughput and storage growth are the same quantity**:
both scale as `cap ÷ spacing`, so at a fixed traffic shape, 200 GB/year *is*
5.6 tx/s. No combination of cap and spacing buys throughput. The only lever that
does is bytes per transaction — and 83% of the adversarial excess is chainstate,
which is UTXO count, not transaction bytes.

So the correct response to criterion 2 is **to price UTXO creation**, not to
shrink blocks. Every candidate in the superseded table below bought its storage
figure by cutting throughput 2–4×, and none of them addressed the actual driver.

### The size term must be additive, not a maximum

With `cost = base × max(weight/R, utxo_delta/U)` the UTXO term is inert: the
adversarial transaction is already weight-heavy enough that the weight term
dominates at any `U` that leaves honest users alone. With
`cost = base × (weight/R + utxo_delta/U)` the attacker pays for both, so the
cheapest way to buy bytes is to create no UTXOs, and adversarial growth collapses
toward the blocks-and-undo floor of ~1.0–1.2 MB/block (104–128 GB/year).

`U` is bounded on both sides by measurement. An honest 1,291-weight two-output
transaction pays `1291/18957` = 0.068 of base on the weight term, so `U` must be
well above ~15 for the UTXO term not to burden it; the adversarial transaction
pays 20.9 on the weight term and creates 2,299 UTXOs, so `U` below ~110 makes the
UTXO term bind for bloat. Anywhere in **30–100** satisfies both. Calibrating
inside that window is Stage 2 work.

### An open hole: weight is not bytes

Weight per serialized byte measured 3.67 for two-output transactions and 3.99 for
2,300-output ones — outputs are non-witness data charged at 4×, signatures are
witness data charged at 1×. The theoretical floor is **1.0**: an all-witness
transaction puts 4 MB of serialized bytes into a 4 MB weight block, ~4× the
honest byte cost, while paying the same weight-denominated work.

A weight-denominated size term does not close that. The observed range is narrow,
but the range is attacker-controlled and was never probed adversarially. Stage 2
should either price serialized bytes directly or bound the ratio. **This is the
weakest point in the recommendation below.**

### Superseded: what shrinking the cap would have bought

Retained because it is what the cap and spacing levers actually do, if they are
ever needed for a reason other than criterion 2. Figures use the adversarial
shape, scaled linearly with the cap.

| Cap | Spacing | GB/year | vs 200 |
| --- | --- | ---: | --- |
| 4 MB | 300 s | 426.3 | FAIL |
| 4 MB | 600 s | 213.1 | FAIL |
| 2 MB | 300 s | 213.1 | FAIL |
| 2 MB | 600 s | 106.6 | PASS |
| 1 MB | 300 s | 106.6 | PASS |
| 1 MB | 600 s | 53.3 | PASS |

Note also that 600 s spacing was a worse idea than it looked:
`nBootstrapBlocks` = 1,051,920 is denominated in **blocks**, commented "10 yr @
5-min" (`chainparams.cpp:100`), so doubling spacing silently doubles the
bootstrap to 20 years and halves annual issuance.

---

## The coupling the spec did not anticipate: capacity is an issuance lever

Only relevant if the block cap moves. It does not, under the recommendation
below, so the monetary schedule is untouched — but the coupling is recorded
because it is what makes shrinking the cap expensive.

Mint is charged per transaction, so shrinking the cap reduces how many
transactions a full block holds and therefore how much it mints. Writing G for
full-block mint divided by the tail subsidy, and holding `nTxPowMint`:

    G = cap / 1,958,247        (using M4's lightest 1,119-weight transaction)

Today's 4 MB gives G = 2.04, the "mints 2x the tail subsidy" design property.
Halving the cap halves G. Restoring G by raising `nTxPowMint` puts `C` in the
denominator of the mint-safety ceiling, so it costs margin one-for-one:

    mint-safety margin x G = cap / 474,456

**Holding `nTxPowMint` preserves the full 4.13x margin at any cap** — the margin
depends only on `C` and `r`, not on the cap. So the cap trades against G alone,
and G against margin alone.

## The admissible candidate

**One combination satisfies all five criteria, and it changes no parameter that
was already chosen:**

| Parameter | Value | Change |
| --- | --- | --- |
| `nTxEdgeBits` | 29 | **unchanged** |
| `MAX_BLOCK_WEIGHT` | 4,000,000 | **unchanged** |
| `nPowTargetSpacing` | 300 s | **unchanged** |
| `nTxPowMint` | 57,143 | **unchanged** |
| `K` | 106 | **unchanged** |
| Tail schedule (S0=50, tail=1, N=1,051,920) | | **unchanged** |
| Size term in `RequiredTxWork` | weight against `R` = 18,957 | **new** |
| UTXO-delta term in `RequiredTxWork` | additive, `U` in 30-100 | **new** |
| Per-block mint cap | 2.00 COIN | **new** |

Criterion by criterion:

1. **Fill >= mine.** 1.00 at every integer mean block work from 2 to 100,000,
   verified by scan, with `R` = 18,957 at the 4 MB cap. A size term is
   **necessary** — without one the ratio is `n_max/K` = 0.094.
2. **Storage.** Honest traffic measured at **152.0 GB/year**, 24% inside the 200
   ceiling, at full 10.33 tx/s. The UTXO-delta term is what keeps the adversarial
   shape from reaching 426; with it, the attacker's cheapest bytes carry no UTXOs
   and growth tends toward the 104-128 GB/year blocks-and-undo floor.
3. **Mint safety.** Ratio 0.0606 against alpha = 0.25 — the full **4.13x margin**,
   untouched, because neither `nTxPowMint` nor `nTxEdgeBits` moves.
4. **Issuance bounded.** The 2.00 COIN per-block cap makes maximum mint
   independent of transaction size, closing the 357x span M4 measured. Max mint
   at the lightest 1,119-weight transaction is 2.042 COIN, so the cap binds
   marginally and immediately.
5. **Honest sender cost.** **1.70 min on GPU, 35.4 min on the 8-thread reference
   desktop.** Unchanged, because `nTxEdgeBits` does not move.
   **Superseded 2026-08-02:** it did move, together with `nEdgeBits`, to 28. The
   shipped cost is **0.83 min on GPU, 15.8 min on CPU** and the margin is still
   4.13×. See the frame note under criterion 3 — Stage 1 held the block-PoW size
   fixed, and this conclusion is scoped to that.

### Rejected

| Candidate | Failed | Measured |
| --- | --- | --- |
| `nTxEdgeBits` 22-26 | 3 | mint-safety ratio 0.59 - 13.6 against alpha = 0.25 |
| `nTxEdgeBits` 27 | 3 | 0.2646 against 0.25 - misses by 6% |
| `nTxEdgeBits` 28 | 3 | passes at 2.00x, but spends half the margin for a 2x cheaper grind — **against a FIXED E29 block size. Moving both sizes to 28 together keeps r = 1 and the full 4.13x margin, and is what shipped.** |
| Any cap/spacing reduction | — | buys storage only by cutting throughput 2-4x, and does not touch the UTXO driver |
| Size term without a UTXO term | 2 | 426.3 GB/year at the adversarial shape |
| `max()` rather than additive pricing | 2 | UTXO term inert; weight term already dominates |

### The tightest bound available

Criterion 2 asks for the tightest storage bound the other four criteria allow.
With the UTXO term pricing bloat, the binding figure is honest traffic at
**152 GB/year**, and the floor if the attacker avoids UTXOs entirely is
**~104-128 GB/year**. Tightening further would mean cutting the cap, which costs
throughput and G without addressing any measured driver.

## What Stage 1 could not price

Criterion 1 compares two attacker costs, both on attacker hardware. The
P102-100 the timings were taken on has an outright price ($64.99) but **no rental
market** — it is a mining-only Pascal part no cloud provider offers. The parity
condition is a ratio of two costs on the same card, so it is unaffected; what is
unpriced is the absolute dollar cost of sustaining the attack, which would say
how much margin beyond parity is worth buying. Closing it needs one timing run on
a rentable card.
