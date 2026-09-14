# Block PoW with Cuckatoo

Quicksilver block proof-of-work uses Cuckatoo. A block is valid only when its
header carries a valid 42-edge Cuckatoo cycle and that cycle's proof hash meets
the compact target encoded in `nBits`.

## Header Format

The block header is the 84-byte pre-pow prefix plus a serialized proof tail:

```
int32   nVersion
uint256 hashPrevBlock
uint256 hashMerkleRoot
uint32  nTime
uint32  nBits
uint32  nCongestion
uint32  nNonce
uint32  nCycle[42]
```

The first 84 bytes through `nNonce` seed the Cuckatoo SipHash keys. `nCycle` is
serialized after `nNonce` as 42 little-endian `uint32` values in ascending order.
The block hash commits to the full serialized header, including `nCycle`.

`nCongestion` precedes `nNonce` rather than following it, so `nNonce` stays the
trailing four bytes of the pre-pow. Every solver grinds that tail in place, so the
ordering is a correctness requirement, not a stylistic one. Putting the multiplier
inside the pre-pow is also what stops a miner restating it on an already-solved
block: changing it changes the SipHash keys and invalidates the cycle.

## Validation

Block PoW validation requires both predicates:

```
CuckatooVerify(nCycle, keys_from_84_byte_pre_pow, nEdgeBits)
CuckatooProofHash(nCycle) <= target_from_nBits
```

The target and retarget machinery continue to use compact target semantics. The
value being compared is the Cuckatoo proof hash rather than a hash of the 84-byte
header prefix.

## Parameters

`nEdgeBits` is a consensus parameter. Sandbox can use a smaller graph for fast
local testing, while public networks can use production-sized graphs from the
same binary.

The shipped values are **28 on `main` and `publictest`** (moved from 29 on
2026-08-02) and **19 on `sandbox`**.

`nEdgeBits` always equals `nTxEdgeBits` on the public networks, and that is a
load-bearing property rather than a coincidence. One graph size means one solver
serves both block PoW and per-transaction PoW, so `r` — the transaction-solver
rate relative to the block-solver rate — is 1.00 by construction. The Frame-B
mint-safety derivation depends on `r` = 1; splitting the two sizes invalidates it.
See `doc/audit/mainnet-difficulty-floor-model.md`.

**Difficulty comes from the target, not from the graph size.** Cycles-per-block is
`2^256 / target`, so a larger graph does not mean a harder block — it means a
slower solver. That separation is what lets the graph size move without touching
the difficulty law: when the size moved 29 → 28 and each cycle became 2.06x cheaper
to find, the floor moved 2 → 4 cycles to hold the *work* per block constant. The
floor is denominated in real work, not in `nBits`.

The genesis block is exempt by hash because it is a compile-time asserted trust
anchor. Non-genesis blocks must satisfy normal Cuckatoo PoW rules.

## Mining

Block mining varies `nNonce`, derives keys from the 84-byte pre-pow prefix, solves
for a 42-cycle, then checks the proof hash against the target. If the proof hash
does not satisfy the target, the miner changes the nonce, time, or coinbase
extranonce and tries again.

External miners should use the `pow` object in
[external-mining.md](../mining/external-mining.md) rather than assuming
SHA256d-style nonce grinding.
