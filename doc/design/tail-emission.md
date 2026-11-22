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

## A ceiling, not a schedule

The curve gives the most a block may claim, not what it must claim. Consensus
rejects a coinbase that pays more than the block's subsidy plus its
per-transaction mint allowance, and accepts one that pays less. The difference
is never created. A node started with `-minezeroclaim` mines blocks that claim
nothing.

So the curve, summed over heights, is an upper bound on supply. The coins that
exist are the ones the chain's blocks actually claimed.

## Properties

- height 0 pays the initial subsidy;
- the final bootstrap blocks approach the tail;
- every height at or after the bootstrap boundary pays exactly the tail subsidy;
- the sequence is monotonic non-increasing;
- each per-block subsidy must stay within `MoneyRange`.

The tail means Quicksilver keeps issuing indefinitely. Over time, issuance remains
bounded per block while the total supply can keep growing, so the most the
percentage issuance rate can be trends downward.
