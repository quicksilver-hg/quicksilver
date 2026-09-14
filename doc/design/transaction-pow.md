# Transaction PoW

Every non-coinbase Quicksilver transaction carries a Cuckatoo proof. Nodes verify
that proof during relay pool admission and again during block connection.

Transaction PoW is an admission and anti-spam rule. It does not directly mint
coins; Frame-B minting consumes the same validated work at the block reward
ceiling.

## Carriage

Transaction PoW is serialized in the base transaction data so it is part of the
transaction id:

```
nAnchorHeight
nPowNonce
nCycle[42]
```

Coinbase transactions are exempt. All other transactions must carry a proof that
validates under the transaction PoW parameters for the relevant chain context.

## Preimage

The proof preimage binds to transaction intent:

- version;
- inputs by previous output and sequence;
- outputs;
- lock time;
- anchor height;
- PoW nonce.

It excludes script signatures, witness data, and the cycle itself. That lets a
vault grind the proof on the unsigned transaction body and then sign without
invalidating the proof.

## Anchors

Transaction PoW is anchored to recent chain context. The anchor prevents old
proofs from remaining mineable indefinitely and lets the required work floor
track chain conditions. A transaction whose anchor ages out is no longer mineable
and should be evicted from the relay pool with its descendants.

## Validation

For non-coinbase transactions, validation checks:

```
CuckatooVerify(nCycle, keys_from_tx_pow_preimage, nTxEdgeBits)
CuckatooProofHash(nCycle) <= GetTxPowTarget(anchor_context)
```

The same predicate gates relay pool admission, block connection, and Frame-B mint
eligibility.
