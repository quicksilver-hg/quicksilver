# Stage 2 — byte-denominated pricing, the slicing attacker, and `U`

- Date: 2026-08-02
- Git commit: `fd08081`
- Command: `cd tools/calibration && python3 -m pytest test_model.py`, plus the
  table generator recorded below
- Produced by: `tools/calibration/model.py` (`tx_required_work`,
  `fill_to_mine_ratio_sliced`, `utxo_term_share`,
  `adversarial_growth_gb_per_year`)
- Inputs: M5 shapes (honest 1,291 weight at 3.67 weight/byte; adversarial
  99,228 bytes carrying 2,300 outputs; 121.1 bytes of chainstate per UTXO),
  `K` = 106, `R_b` = 4,739, spacing 300 s, block cap 4 MB of weight

All numbers below were printed by running the functions. None were transcribed.

## Why the rounding direction is a finding, not a detail

Spec §13.3 fixes the rule as one division but does not say which way it rounds.
Under **floor** division an attacker slices their bytes into transactions of
`2·R_b − 1` = 9,477 bytes. Each floors to one unit of base while carrying
nearly two, so 1 MB of block bytes costs 106 units where honest arithmetic
charges 211 — criterion 1 at **0.5024**. That is the `W = 2K − 1` pathology
Stage 1 found in `base`, reappearing one level down inside the fix for it.
A 100,000-byte slice fails too, at 0.9953.

Under **ceiling** division the sum of per-transaction charges is at least
`bytes/R_b` for every slicing, because the sum of ceilings is never below the
ceiling of the sum. The worst case anywhere in the table below is 1.0047.

## Criterion 1 — fill ÷ mine, by mean block work and attacker slice

**Rounding: floor — REJECTED**

| mean block work | 237 B | 4,738 B | 4,739 B | 9,477 B | 9,478 B | 100,000 B | 1,000,000 B |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2110.0000 | 106.0000 | 106.0000 | 53.0000 | 105.5000 | 105.0000 | 105.5000 |
| 105 | 40.1905 | 2.0190 | 2.0190 | 1.0095 | 2.0095 | 2.0000 | 2.0095 |
| 211 | 20.0000 | 1.0047 | 1.0047 | 0.5024 | 1.0000 | 0.9953 | 1.0000 |
| 1,060 | 3.9811 | 1.7925 | 1.9915 | 1.8915 | 1.9906 | 1.9906 | 1.9906 |
| 10,600 | 1.9903 | 1.9711 | 1.9907 | 1.9809 | 1.9907 | 1.9906 | 1.9907 |
| 100,000 | 1.9831 | 1.9882 | 1.9899 | 1.9890 | 1.9899 | 1.9898 | 1.9899 |

**Rounding: ceiling — SHIPPED**

| mean block work | 237 B | 4,738 B | 4,739 B | 9,477 B | 9,478 B | 100,000 B | 1,000,000 B |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 2110.0000 | 106.0000 | 106.0000 | 106.0000 | 106.0000 | 110.0000 | 106.0000 |
| 105 | 40.1905 | 2.0190 | 2.0190 | 2.0190 | 2.0190 | 2.0952 | 2.0190 |
| 211 | 20.0000 | 1.0047 | 1.0047 | 1.0047 | 1.0047 | 1.0427 | 1.0047 |
| 1,060 | 3.9811 | 1.9915 | 1.9915 | 1.9915 | 1.9915 | 2.0000 | 1.9915 |
| 10,600 | 2.3884 | 1.9911 | 1.9908 | 1.9909 | 1.9908 | 1.9915 | 1.9908 |
| 100,000 | 2.0253 | 1.9903 | 1.9899 | 1.9901 | 1.9899 | 1.9899 | 1.9899 |

The full scan — every integer mean block work from 2 to 100,000 against
all seven slice sizes — reports **zero violations** under ceiling
division. See `test_criterion_1_holds_across_the_whole_range_under_ceiling_division`.

## Criterion 2 — adversarial growth by U

| U | GB/year | vs the 200 GB/year ceiling |
| ---: | ---: | :--- |
| 30 | 43.1 | inside |
| 50 | 62.9 | inside |
| 100 | 95.8 | inside |
| 500 | 164.8 | inside |

## The admissible window for U

Lower bound: the UTXO term must stay a minority of an honest payment's
bill (share ≤ 0.5). Upper bound: it must be the larger half of the bloat
shape's bill (share ≥ 1.0), or it is not pricing what it exists to price.

| U | honest UTXO share | bloat UTXO share | growth GB/yr | admissible |
| ---: | ---: | ---: | ---: | :--- |
| 15 | 0.900 | 7.32 | 24.2 | no |
| 25 | 0.540 | 4.39 | 37.3 | no |
| 28 | 0.482 | 3.92 | 40.8 | yes |
| 30 | 0.450 | 3.66 | 43.1 | yes |
| 50 | 0.270 | 2.20 | 62.9 | yes |
| 100 | 0.135 | 1.10 | 95.8 | yes |
| 109 | 0.124 | 1.01 | 100.1 | yes |
| 110 | 0.123 | 1.00 | 100.6 | no |
| 150 | 0.090 | 0.73 | 116.0 | no |

**Admissible window: U in 28–109. Shipped value: 50.**

This reproduces the 30–100 window `feasible-region.md` reports, from the
same two constraints, without having been fitted to it.

## The two shapes, priced

- Honest two-output transaction: 351 serialized bytes, byte term 0.074 of base, UTXO term 0.020 of base. At the launch floor it pays 1 cycle.
- Adversarial transaction: 99,228 bytes carrying 2,300 outputs, byte term 20.94 of base, UTXO term 45.98 of base. The UTXO term is 2.20x the byte term — which is the point of it.
