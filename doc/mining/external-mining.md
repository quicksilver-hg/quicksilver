# Quicksilver — External Block Mining (getblocktemplate)

This guide documents how an external miner builds, solves, and submits a Quicksilver
block using the `getblocktemplate` (GBT) / `submitblock` RPCs. Quicksilver's block
proof-of-work is **Cuckatoo** (not SHA256d), and the chain is **feeless** with a
**Frame-B per-tx mint**. The GBT response is BIP22/23-compatible plus a self-contained
`pow` object and a coinbase breakdown that describe these differences.

> Per-transaction PoW is **not** the miner's job. Each transaction already carries its
> own Cuckatoo proof (the user's vault grinds it). The block miner selects the
> template's transactions as-is and solves only the **block** PoW.

## 1. Request

```
getblocktemplate {"rules": ["segwit"]}
```

## 2. The `pow` object (block PoW descriptor)

```json
"pow": {
  "algorithm": "cuckatoo",
  "edgebits": 28,            // graph size (log2 edges); 28 on main and publictest, 19 on sandbox
  "proofsize": 42,           // cycle length
  "proofhash": "blake2b",    // hash applied to the cycle
  "proofhashtarget": "00000000ffff...",  // CuckatooProofHash(nCycle) must be <= this
  "prepowsize": 84,          // header bytes (through nNonce) that seed the siphash keys
  "cycleencoding": "uint32le x42 ascending, appended to the 84-byte header",
  "congestionparams": {      // inputs for recomputing nCongestion yourself (see §3.1)
    "one": 65536,            // fixed-point scale: this value means multiplier 1.0
    "targetpermille": 500,   // target block fullness T, in permille of weightlimit
    "stepdenom": 4,          // max per-block step is 1/stepdenom
    "maxmultiplier": 64      // ceiling, in units of "one"
  },
  "trivialcycle": true       // sandbox only: no real 42-cycle required (see §6)
}
```

The template also carries a top-level `congestion` field alongside `bits` — the
`nCongestion` value for **this template's transaction set**. See §3.1.

`pow.proofhashtarget` equals the top-level `target` field; both are the
compact-decoded `bits`. `target` and `bits` are kept for BIP22 back-compat, but
`pow` is authoritative for this chain.

There is no `noncerange`. BIP22 uses it to hand a hashing miner its nonce search
space, and a Quicksilver block is not found that way: `nNonce` seeds the siphash
keys for a Cuckatoo graph and the proof is the 42-edge cycle, so advertising a
32-bit sweep would describe work no miner on this chain performs. Read `pow`.

## 3. The block header (252 bytes)

The serialized header is an 84-byte pre-pow prefix **plus** the 42-edge cycle:

```
offset  size  field
0       4     nVersion        (int32 LE)
4       32    hashPrevBlock
36      32    hashMerkleRoot
68      4     nTime           (uint32 LE)
72      4     nBits           (uint32 LE)
76      4     nCongestion     (uint32 LE)        <-- congestion multiplier, see §3.1
80      4     nNonce          (uint32 LE)        <-- end of the 84-byte pre-pow prefix
84      168   nCycle[42]      (uint32 LE each, ascending)
```

- The **pre-pow** is bytes `[0, 84)` (through `nNonce`). It seeds the Cuckatoo siphash
  keys. `nCycle` must **not** feed back into the keys.
- `nNonce` is deliberately the **last four bytes** of the pre-pow. `nCongestion` was
  inserted before it, not appended after it, precisely so that a solver which grinds
  `buf[len-4 .. len-1]` in place keeps grinding the nonce. If you wrote a solver against
  the old 80-byte layout, widening the buffer to 84 is the only change it needs.
- `GetHash()` (the block hash / `previousblockhash` of the next block) is
  `SHA256d` of the **full 252-byte** header (including `nCycle`).

### 3.1 `nCongestion` — you may have to compute it yourself

`nCongestion` is the EIP-1559 congestion multiplier, in fixed point against
`pow.congestionparams.one`. It is consensus-checked: a node recomputes it at connect
time from the parent block's multiplier and **your block's final weight**, and rejects a
mismatch as `bad-congestion`. "Final weight" means the block as you submit it — nothing
is added to it on the node's side, including the coinbase witness commitment (§5.1).

The template's `congestion` field is correct **only if you mine the template's
transaction set unchanged**. `transactions` is in the `mutable` list, so if you add,
drop, or reorder transactions — or supply a coinbase of a different size — you must
recompute:

```
target_weight = weightlimit * targetpermille / 1000
diff          = abs(your_block_weight - target_weight)
delta         = parent_m * diff / target_weight / stepdenom
next_m        = your_block_weight > target_weight ? parent_m + delta
                                                  : parent_m - min(parent_m, delta)
nCongestion   = clamp(next_m, one, maxmultiplier * one)
```

All integer arithmetic, all truncating division — floating point will round differently
from consensus and produce blocks that are rejected only sometimes. `parent_m` is the
`nCongestion` of the block named by `previousblockhash`.

Because `nCongestion` is inside the pre-pow, **set it before you grind**. Changing it
afterwards invalidates the cycle.

## 4. The solve loop

```
1. Assemble the header: nVersion, hashPrevBlock = previousblockhash, hashMerkleRoot
   (from your coinbase + the template transactions), nTime = curtime, nBits = bits,
   nCongestion = the template's `congestion` (or recomputed per §3.1 if you changed the
   body), nNonce = 0.
2. Derive the Cuckatoo siphash keys from the 84-byte pre-pow.
3. Solve Cuckatoo at `pow.edgebits` for a 42-cycle (ascending edge indices).
4. Compute the proof hash: blake2b-256 over the 42 edges packed little-endian uint32,
   interpreted little-endian. If proofhash <= proofhashtarget -> done.
   Otherwise bump nNonce and go to step 2.
```

`proofhashtarget` controls block difficulty; the network retargets `bits` to hold the
~5-minute block interval.

## 5. The coinbase (Frame-B)

```json
"coinbasevalue": 5000057143,   // total the coinbase may claim (cinnabar)
"subsidy":       5000000000,   // base block subsidy
"mintvalue":         57143     // Frame-B per-tx mint sum (ΣC over the anchor-valid txs)
```

- `coinbasevalue == subsidy + mintvalue`. Your coinbase output must pay **at most**
  `coinbasevalue`; paying exactly `coinbasevalue` claims the full mint allowance.
- `mintvalue` is the sum of the per-tx mint (`nTxPowMint`) over the template's
  anchor-valid transactions — it grows with the number of included minting txs.

### 5.1 The witness commitment — the node will not fill it in for you

Your coinbase must carry the witness commitment itself. Two pieces, and both are yours
to supply:

1. **The commitment output.** Append an `OP_RETURN` output whose scriptPubKey is the
   template's `default_witness_commitment`. That value is only valid for the template's
   transaction set — change the set and you must recompute it over your own witness
   merkle root.
2. **The witness reserved value.** The coinbase input's witness stack must hold
   **exactly one 32-byte item**. `default_witness_commitment` is computed against the
   all-zero value, so unless you are computing the commitment yourself, that item is
   **32 zero bytes**.

The node validates the block exactly as you submitted it and does not repair the
coinbase. Omitting the reserved value, or supplying one of the wrong size, is rejected
as `bad-witness-nonce-size`; a commitment that does not match your witness merkle root
is `bad-witness-merkle-match`.

⚠ Both pieces are part of the block's weight, so add them **before** you compute
`nCongestion` (§3.1) and grind. This is the ordering that matters most: a node that
quietly completed your coinbase would be weighing a different block than the one you
ground, and would reject your correctly-computed multiplier as `bad-congestion`.

## 6. Feeless / transaction selection

- The chain is **feeless**: transactions preserve value exactly (`in == out`) and
  GBT transaction entries do not carry a monetary transaction-fee field.
- Inclusion is ranked **node-side** by per-tx surplus-work; the template already
  contains the chosen, ordered set. Include the template's transactions **as-is** —
  each already carries its own Cuckatoo proof tail
  (`nAnchorHeight + nPowNonce + nCycle`, 176 bytes after `nLockTime`).

## 7. Submit

```
submitblock "<full block hex>"      # header (incl. nCycle) + all transactions
```

Returns `null` on acceptance, `"duplicate"`, or a BIP22 reject reason.

`getblocktemplate` proposal mode (`{"mode": "proposal", "data": "<block hex>"}`)
validates the template **structure** (transactions, merkle root) and intentionally
**skips** the block PoW per BIP23 — use it to pre-validate an assembled template; the
cycle is validated at `submitblock`.

## 8. Sandbox worked example (`trivialcycle: true`)

On sandbox the block PoW is trivial (`fBlockPowNoCycle`): no real 42-cycle is required,
only that the proof hash clears the target. A miner can solve it in a few lines:

```python
import hashlib

def proof_hash_int(cycle):
    buf = b"".join(int(e & 0xffffffff).to_bytes(4, "little") for e in cycle)
    return int.from_bytes(hashlib.blake2b(buf, digest_size=32).digest(), "little")

def solve_trivial(target_int):
    cycle = list(range(1, 43))          # [1..42]
    while proof_hash_int(cycle) > target_int:
        cycle[0] = (cycle[0] + 42) & 0xffffffff
    return cycle

# target_int = int(tmpl["pow"]["proofhashtarget"], 16)
```

A complete round-trip (GBT → assemble → solve → submitblock) is exercised by
`test/functional/feature_quicksilver_gbt.py`, which mirrors this flow against a real
sandbox node.
