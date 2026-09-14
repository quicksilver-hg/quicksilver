# Tail Emission

Quicksilver's base subsidy is a deterministic height function with a bootstrap
ramp and a perpetual tail.

## Curve

The subsidy starts at an initial value, decreases linearly across the bootstrap
window, and then remains at the tail value forever:

```
if height >= bootstrap_blocks:
    subsidy = tail_subsidy
else:
    subsidy = initial_subsidy
              - ((initial_subsidy - tail_subsidy) * height) / bootstrap_blocks
```

The implementation uses integer arithmetic and a widened intermediate for the
multiplication so every node computes identical values without overflow.

## Parameters

The curve is parameterized by:

- `nInitialSubsidy`;
- `nTailSubsidy`;
- `nBootstrapBlocks`.

The finalized public-network values are an initial subsidy of 50 COIN, a
perpetual tail subsidy of 1 COIN, and a bootstrap window of 1,051,920 blocks
(ten years at the five-minute target spacing). Sandbox uses a 150-block window
so tests cross the tail boundary quickly while exercising the same curve.

## Properties

- height 0 pays the initial subsidy;
- the final bootstrap blocks approach the tail;
- every height at or after the bootstrap boundary pays exactly the tail subsidy;
- the sequence is monotonic non-increasing;
- each per-block subsidy must stay within `MoneyRange`.

The tail means Quicksilver keeps issuing indefinitely. Over time, issuance remains
bounded per block while the total supply grows, so the percentage issuance rate
trends downward.
