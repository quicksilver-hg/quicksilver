# Vault and Mining Glue

Vault and mining glue connects Quicksilver's consensus rules to ordinary node
operation: vaults produce exact-value transactions with transaction PoW, the
relay pool keeps only mineable anchored transactions, and block assembly claims the
correct subsidy plus mint allowance.

## Vault Sends

Vault construction follows this order:

1. choose inputs and outputs so input value equals output value;
2. grind transaction PoW for the finalized unsigned body;
3. sign the transaction.

The PoW preimage excludes signatures and witness data, so signing after grinding
does not invalidate the proof.

## Anchor Freshness

Transaction PoW anchors expire. Block assembly must skip stale-anchor
transactions because they would fail block connection. Relay pool maintenance should
remove stale-anchor entries with descendants after new blocks connect.

This is not a second consensus rule; it is node hygiene that keeps the mining
path from assembling invalid blocks and keeps the relay pool focused on transactions
that can still be mined.

## Mint Claim

The block assembler sets the coinbase value from the transactions actually in
the candidate block:

```
coinbase value = subsidy + GetBlockMintAllowance(block)
```

The allowance is recomputed during validation, so a malformed or stale
transaction cannot inflate the coinbase claim.

## Solver Boundary

GPU or external solver paths are mining accelerators only. Any proof returned by
an accelerator must be self-verified before the node accepts it for block or
transaction construction. Consensus verification never depends on trusting the
accelerator.
