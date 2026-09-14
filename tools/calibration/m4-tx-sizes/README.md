# M4 — real transaction sizes and implied mint per block

- Date: 2026-07-31
- Git commit: `c258338` (measurement corrected in the following commit)
- Machine: Intel(R) Xeon(R) CPU E5-1620 v2 @ 3.70GHz (development desktop)
- Command: `cd build/test/functional && ./calibration_tx_sizes.py`
- Network: sandbox, `-txpownocycle=1` (trivial tx-PoW; the 172-byte PoW tail is
  present either way, so sizes are unaffected)
- Produced by: `test/functional/calibration_tx_sizes.py`

## What this replaces

The spec retired an invented "~394 bytes per transaction". `nTxPowMint = 57,143`
cinnabar was derived assuming roughly 1,143 bytes per transaction. This measures
the real sizes.

## Capacity is bound by weight, not serialized size

`consensus.h:13` documents `MAX_BLOCK_SERIALIZED_SIZE` as "only for buffer size
limits". The network rule is `GetBlockWeight(block) > MAX_BLOCK_WEIGHT`
(`validation.cpp:4130`). The spec agrees implicitly: finding 3.3's
`n_max = 4,000,000 / 400,000 = 10` is a ratio of weights.

**A first run of this measurement divided by serialized bytes instead**, which
overstated capacity by ~3.6x and produced a 3.7x mint drift that is not real.
The script now asserts that the near-maximum-standard row yields exactly
`n_max = 10`, so this specific error cannot recur silently.

## Result

| label | vsize | weight | serialized bytes | tx/block | mint/block (COIN) | bytes per full block |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| one_in_one_out | 280 | 1,119 | 309 | 3,574 | 2.042 | 1,104,366 |
| one_in_two_out | 323 | 1,291 | 352 | 3,098 | 1.770 | 1,090,496 |
| near_max_standard | 100,000 | 399,999 | 100,029 | 10 | 0.006 | 1,000,290 |

## Findings

1. **Mint drift at the light end is 1.02x, not 3.7x.** A block filled with
   minimum-size transactions mints **2.042 COIN** against the 2.00 design
   intent. `nTxPowMint = 57,143` is well calibrated for small transactions
   despite having been derived from a wrong size assumption — the two errors
   (assumed bytes, and bytes vs weight) very nearly cancel.

2. **The exposure is at the heavy end, not the light one.** A block filled with
   near-maximum standard transactions mints 0.006 COIN — a **357x spread**
   across ways of filling one block. Criterion 4 asks that issuance not depend
   on transaction size; it depends on it by a factor of 357, but the deviation
   is a shortfall, not an over-issue.

3. **A weight-full block of realistic transactions is ~1.1 MB, not 4 MB.** Every
   row lands near 1.0–1.1 MB per full block, because these transactions carry
   ~3.6 weight per serialized byte. Chain-growth figures computed from a 4 MB
   block therefore overstate honest-usage growth by roughly 3.6x.

4. **But 1.1 MB is not the storage ceiling.** Weight is `3 x base + total`, so a
   witness-heavy transaction approaches 1 weight per byte. An adversary
   optimising for bytes-on-disk rather than transaction count can push toward
   ~4 MB per block while staying inside the weight cap. The honest-usage growth
   figure and the adversarial ceiling differ by ~3.6x and **both** belong in the
   model — M5 measures what a filled block costs, but the fill it measures is
   the realistic one.

## Caveats

- `MiniVault` transactions are anyone-can-spend (`ADDRESS_OP_TRUE`). A real
  single-sig spend carries a signature and would be somewhat heavier, giving
  slightly fewer transactions per block and slightly less mint. 1,119 weight is
  therefore a floor on weight and a ceiling on mint drift.
- `bytes_per_full_block` is `txs_per_block x serialized_bytes` and excludes the
  coinbase and block header — a few hundred bytes against ~1.1 MB.
- Finding 4 is reasoned from the weight formula, not measured. A witness-stuffed
  transaction was not built here; if the model leans on the ~4 MB adversarial
  ceiling, that case should be measured rather than assumed.
