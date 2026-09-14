# Retired-Chain Cleanup

Quicksilver removed public surfaces for inherited auxiliary chain modes that were
not part of the launch product.

## Outcome

User-facing options, generated docs, installer examples, seed data, and test
fixtures were cleaned so public readers and operators see only current
Quicksilver network modes.

The cleanup preserved sandbox because it remains the local development and test
network.

## Verification

The stale-chain lint now scans public docs, generated examples, contrib tooling,
and functional tests for retired chain names and removed command-line switches.
The expected result is no tracked public references outside negative tests that
assert removed options fail.
