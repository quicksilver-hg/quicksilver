# Economics and Consensus

Quicksilver is a fresh-chain, feeless UTXO network with two proof-of-work layers:

- **Block PoW** orders and secures the chain.
- **Transaction PoW** is carried by every non-coinbase transaction and provides
  the admission cost that replaces monetary transaction fees.

Both layers use Cuckatoo. The block layer targets a 5-minute cadence. The
transaction layer uses a smaller Cuckatoo graph so users and vaults can produce
proofs while nodes can verify them cheaply.

## Feeless Model

A normal transaction must preserve value exactly:

```
input value == output value
```

`input value < output value` remains invalid as value creation. `input value >
output value` is also invalid and is rejected as `bad-txns-not-feeless`.

The block miner is paid by:

```
base subsidy at block height + Frame-B mint allowance
```

There is no monetary transaction-fee term in the reward ceiling.

## Frame-B Mint

Each non-coinbase transaction with a fresh, valid transaction PoW authorizes a
capped mint `C` to the block miner. The mint is bound to work, not to
transaction value, so moving larger amounts does not authorize larger issuance.

The implementation invariant is:

```
coinbase value <= subsidy(height) + sum(C for mint-qualifying transactions)
```

Transactions never create or destroy value directly. New coins enter only through
the coinbase reward.

## Congestion

Congestion is handled by work rather than by fees. Every transaction must meet a
required work floor derived from its anchor and recent chain conditions. When
transactions compete for block space, nodes rank them by surplus work per vsize.

Surplus work buys queue position, not extra mint value. The mint remains flat per
qualifying transaction, keeping issuance bounded by block capacity.

## Supply

Quicksilver has no hard supply cap. Base subsidy decays through a bootstrap ramp
to a perpetual tail subsidy, and Frame-B minting can continue as long as
transactions carry valid work. Nominal supply therefore grows forever, while the
percentage issuance rate trends downward as supply accumulates.

`MAX_MONEY` is retained only as a technical per-value overflow guard. It is not an
economic cap on cumulative supply.

## Consensus Invariants

- Every non-coinbase transaction preserves value exactly.
- Every non-coinbase transaction carries valid transaction PoW.
- A transaction PoW can authorize at most one Frame-B mint.
- A block coinbase cannot exceed subsidy plus mint allowance.
- Block and transaction PoW verification use the network's consensus parameters.
