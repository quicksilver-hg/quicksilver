# Chain Growth and Transaction Cost

> Status: **designed and landed.** The Stage 2 byte-pricing flag day added the
> rules below before launch. Measurements and the public decision record are in
> [`tools/calibration/`](../../tools/calibration/) and
> [the Stage 2 byte-pricing record](../../tools/calibration/stage2-byte-pricing.md).
> An earlier version of this document was an open problem statement. The problem is
> the same; the answer is now written down.

## 1. Why these are one problem

This document once described two costs — the chain growing faster than an ordinary
participant can store, and a transaction costing more work than an ordinary machine
can do. They look independent. They are not.

Bitcoin's fee market is what makes blocks fill: fees clear demand for space.
Quicksilver has no fees, so per-transaction proof-of-work is the *only* thing
limiting block fill. The transaction proof-of-work price is therefore simultaneously
the anti-spam price and the chain-growth governor. Any decision that makes sending
cheaper is also a decision to make filling the chain cheaper, by the same factor.

## 2. What was wrong

Three findings, read from the source and then measured.

**Required work had no size term.** `RequiredTxWork` was `mean_block_work / K`,
scaled by the congestion multiplier and floored at one cycle. Nothing in it
referenced transaction size, so a transaction cost the same one cycle whether it was
400 bytes or 100 kilobytes. About ten maximum-size standard transactions fill a 4 MB
block — roughly ten cycles to add 4 MB to everyone's disk, forever. The protocol
priced transactions; it did not price bytes, and chain growth is a cost in bytes.

**Filling a block settled at a tenth the cost of mining one.** Once mean block work
passes `K` = 106 cycles, filling costs `n_max × block_work / K`, so the ratio settles
at `n_max / K` = 10/106 ≈ **0.094** and stays there at every hashrate above. `n_max`
existed only because cost was charged per transaction rather than per byte, so an
attacker chose it by slicing their bytes into as few transactions as the rules
allowed.

**Issuance depended on transaction size.** `nTxPowMint` = 57,143 was derived as
`2 · COIN / N_tx,max` with `N_tx,max` = 3,500, which bakes an assumed ~1,143-byte
transaction into a consensus constant. `GetBlockMintAllowance` summed it per
anchor-valid transaction with no count cap. M4 measured a 357× span in transactions
per block across realistic shapes.

## 3. What the measurements changed

The 420 GB/year headline this document used to carry was **built from wrong parts**.
It assumed 4 MB of block bytes, where weight-full blocks of realistic transactions
are about 1 MB, and it counted no chainstate at all. The two errors happened to
cancel: M5 measured 426 GB/year for the adversarial shape, within 1.5% of the
headline and arrived at by counting different things.

The finding that matters is what the total is *made of* — **at the UTXO-heavy shapes**,
which is what motivated pricing UTXO creation:

| component | share of on-disk cost, 200–2,300 output shapes |
| --- | ---: |
| chainstate (the UTXO set) | 75–83% |
| blocks and undo files | 24% |
| block index | negligible (320 B/block) |

**At those shapes the binding storage cost is the UTXO set, not block size.** The split
inverts at the honest 2-output shape, where blocks and undo are 84% and chainstate is
16% — which is why pruning is an effective default for an ordinary operator even though
it does nothing about the shapes above. See [chain-storage.md](chain-storage.md) §2.

Honest traffic at the
*unchanged* 4 MB cap and 300 s spacing measures **152.0 GB/year**, 24% inside the
approved 200 GB/year ceiling, at a full 10.33 tx/s. Reducing the block cap or
lengthening block spacing buys a storage number only by cutting throughput by the
same factor — both scale as `cap ÷ spacing` — while leaving the actual driver
untouched.

## 4. The rules that shipped

Every parameter that was already chosen stays chosen: `MAX_BLOCK_WEIGHT` is still
4,000,000, `nPowTargetSpacing` still 300 s, `nTxPowMint` still 57,143, `K` still 106.
The byte-pricing flag day adds rules; it does not retune constants. (`nTxEdgeBits` was
29 when this section was written and is now **28**, moved together with `nEdgeBits` by
the separate E28 flag day described in §5.)

    required = max(1, ceil(base × (bytes·U + max(0, nout − nin)·R_b) / (R_b·U)))

where `base` is `BaseTxWork(anchor)` — the chain-dependent scalar, unchanged — and
`bytes` is `::GetSerializeSize(TX_WITH_WITNESS(tx))`, exactly what an operator writes
to disk, including the segwit marker and flag and the 176-byte transaction PoW tail.

| parameter | value | what it prices |
| --- | ---: | --- |
| `nTxWorkRefBytes` (`R_b`) | 4,739 | serialized bytes that cost one unit of base work |
| `nTxUtxoRefCount` (`U`) | 50 | net new UTXOs that cost one unit of base work |
| `nMaxBlockMint` | 2 × COIN | ceiling on total per-tx mint in one block |

Four properties of that formula are load-bearing, and each has a test.

**Bytes, not weight.** Weight is `4·total − 3·witness`, so it overstates an
attacker's cost by up to 4× in their favour: an all-witness transaction puts 4 MB of
bytes into a 4 MB weight block. The witness discount also exists to subsidise UTXO
consumption in a fee market Quicksilver does not have.

**Additive, not `max()`.** Under `max()` the UTXO term is inert, because the bloat
shape is already byte-heavy. Additive, the attacker pays for both, so the cheapest
way to buy bytes is to create no UTXOs.

**The UTXO delta clamps at zero.** Crediting a negative delta would refund `1/U`
against an input costing about `148/R_b` of base, so at the low end of the admissible
window adding inputs would *lower* required work. Consolidation pays the byte term
only — never penalised, never rewarded.

**The division rounds up, once.** Flooring lets an attacker slice into `2·R_b − 1` =
9,477-byte transactions, each charged one unit while carrying nearly two, and fill a
block for 106 units against the 211 honest arithmetic charges — fill ÷ mine at
**0.5024**. Rounding up charges at least `bytes/R_b` for every slicing, because a sum
of ceilings is never below the ceiling of the sum. Summing two separately-floored
quotients instead of dividing once would lose precision twice.

The mint cap is one clamp inside `GetBlockMintAllowance`, which both the assembler
and the validator already call — so there is no second implementation to keep in sync
and they cannot disagree about it.

## 5. What it costs, and what it buys

| | before | after |
| --- | --- | --- |
| fill ÷ mine, above ~28 GPUs | 0.094 | ≥ 1.00 at every mean block work from 2 to 100,000 |
| adversarial growth | 426.3 GB/year | 62.9 GB/year |
| honest growth at full throughput | 152.0 GB/year | unchanged |
| max mint per block | unbounded by count | 2.00 COIN, independent of transaction shape |
| mint-safety margin vs α = 0.25 | 4.13× | unchanged |
| honest send, 8-thread desktop | 35.4 min | **15.8 min** |
| honest send, one GPU | 1.70 min | **0.83 min** |

(The GPU threshold at which the difficulty floor stops binding was ~57 before the
E28 flag day and is ~28 after it, because each GPU now contributes twice the cycle
rate. The *ratio* in the first row is unaffected — both sides scale together.)

The honest sender's cost halved, and it cost nothing to halve it.

An earlier version of this section argued the opposite: that `nTxEdgeBits` could not
move, because lowering it cheapens minting-by-transaction by the same factor that it
cheapens honest sending. **That reasoning holds only while the block-PoW size is held
fixed.** `r` — the transaction-solver rate relative to the block-solver rate — is a
*ratio* of two solvers. Lowering `nTxEdgeBits` alone drives `r` up and spends the
mint-safety margin, which is what rules out E22–E27 against a fixed E29 block size.
Moving **both** sizes together leaves `r` = 1.00 exactly, by construction: numerator
and denominator scale by the same factor. The margin stays 4.13×, and the sender's
cost halves for free.

That is what the E28 flag day did — `nEdgeBits` and `nTxEdgeBits` moved 29 → 28
together. The rejection of E22–E27 remains correct for the frame it was evaluated
in; it simply does not apply to the unified case. See
[the E28 floor derivation](../../tools/calibration/e28-floor.md) and
`doc/audit/mainnet-difficulty-floor-model.md`.

The four-thread cap in [solve_28.cpp](../../src/crypto/cuckatoo/solve_28.cpp) has been
removed. It bought 17% and changes nothing about viability.

## 6. Sender-side consequence

`PowPreimage` commits to version, prevouts, sequences, outputs, locktime, anchor
height, anchor hash, and the nonce — but **not** to `scriptSig` and not to the
witness, per [transaction.h](../../src/primitives/transaction.h). The grind therefore runs
before the bytes it is charged for exist.

This does not undermine byte pricing, because the charge is computed at validation
from the final transaction. It imposes an ordering rule: the sender grinds against an
**upper bound** on final serialized size. Over-grinding is always valid;
under-grinding yields an invalid transaction, and there is no rounding slack to hide
in because the charge moves with every byte. The vault uses `weight` as that bound —
`weight = 3·base + total`, so `bytes ≤ weight` for every transaction. That
over-charges a witness-heavy transaction by up to 4×, which is invisible below about
400 cycles of mean block work where the one-cycle floor absorbs it, and is recorded
as follow-up work rather than fixed here.

Extending the pre-image to cover the witness was considered and rejected: third-party
witness malleability would then invalidate other people's proofs, which is why
Bitcoin commits to txid rather than wtxid.

## 7. Delegation

Grinding is trustlessly delegatable by construction, for the same reason as above:
the pre-image excludes `scriptSig` and the witness, and the grind runs before
signing, so a third party can perform the work without being able to alter the
payment or steal from it.

What does not exist is any reason for them to. There is no in-protocol reward for
grinding a stranger's transaction, so a grind market requires a payment mechanism
designed from nothing. That is its own piece of work, and it is not this one.

This was taken as a decision on 2026-08-04: **v1 treats a GPU as the practical
transfer path and says so plainly**, rather than shipping a grind market. The
desktop retains a slow CPU fallback; the command-line agent requires the external
solver on live networks. The reasoning, the measured GPU-less cost, and the
condition that would reopen the decision are in
[delegation.md](delegation.md).

## 8. Still undesigned

- ~~**A grind market.**~~ **Decided 2026-08-04:** v1 documents a GPU as the
  practical transfer path. A desktop without a usable GPU still needs about 16
  minutes per transaction — halved by the E28 flag day, from about 35, but not eliminated.
  Delegation is possible; paying for it is not designed, and v1 does not design it.
  See [delegation.md](delegation.md).
- ~~**Who stores the chain.**~~ **Decided 2026-08-04:** consensus nodes prune by
  default, archival storage is the opt-in, and the maintainer runs one archival node at
  launch. What happens if that node disappears is written down rather than mitigated.
  See [chain-storage.md](chain-storage.md).
- ~~**Block-capacity scaling.**~~ **Deferred 2026-08-04 with a measured trigger**, and
  the old framing corrected: block capacity reaches only the congestion multiplier, not
  the `mean_block_work / K` base coupling that actually drives per-transaction latency
  upward. See [v1-scope.md](v1-scope.md) §3.
- **The absolute dollar cost of a sustained fill attack.** The parity condition is a
  ratio of two costs on one card and is unaffected, but the card the timings were
  taken on (P102-100) has no rental market. Closing it needs one timing run on a
  rentable card.

## 9. Related

- [stage2-byte-pricing.md](../../tools/calibration/stage2-byte-pricing.md) — method,
  criteria, and the public decision record
- [feasible-region.md](../../tools/calibration/feasible-region.md) — the Stage 1 working
- [stage2-byte-pricing.md](../../tools/calibration/stage2-byte-pricing.md) — the
  byte-denominated model run
- [transaction-pow.md](transaction-pow.md) — the mechanism itself
- [block-pow-cuckatoo.md](block-pow-cuckatoo.md) — block proof-of-work and its calibration
- [mainnet-difficulty-floor-model.md](../audit/mainnet-difficulty-floor-model.md)
  — the `K` derivation and mint-safety margin
- [agent-client.md](agent-client.md) — who these costs are being lifted off
- [feeless-transactions.md](feeless-transactions.md) — why work replaces fees
