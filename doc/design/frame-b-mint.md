# Frame-B Mint

Frame-B is Quicksilver's miner inclusion incentive for a feeless transaction
system. Users spend electricity to produce transaction PoW; each fresh, valid
transaction proof authorizes a capped mint to the block miner.

This is the sole inflation arithmetic of the coin. A validator that implements
this page incorrectly forks.

## Reward Ceiling

The validation rule is an upper bound on the coinbase value:

```
coinbase value <= GetBlockSubsidy(height) + GetBlockMintAllowance(block)
```

`GetBlockMintAllowance` adds `nTxPowMint` once for each non-coinbase transaction
whose transaction PoW is valid for the block's anchor context, then clamps the
sum to a per-block ceiling:

```
allowance = min(nTxPowMint * (qualifying transactions), nMaxBlockMint)
```

Coinbase transactions never mint for themselves.

A transaction qualifies only if **both** hold: its anchor resolves against the
block's parent (`CheckTxAnchor`), and its proof verifies against the target
derived from that anchor (`CheckTxProofOfWork`). The allowance re-runs the same
predicate that gates inclusion, so it cannot pay for an unproven or
stale-anchored transaction regardless of the order in which a caller does its
checks. Both the block assembler and the validator reach the ceiling through this
one function, so there is no second implementation to keep in sync.

## Per-Block Issuance Ceiling

`nMaxBlockMint` exists because **maximum issuance must not depend on transaction
shape**. Without it, the per-transaction sum is bounded only by how many
transactions fit in a block, and a block packed with minimum-size transactions
mints proportionally more than a block of realistic ones — measured at a 357x
span across realistic transaction shapes. That would leave `nTxPowMint`'s design
property resting on an assumed 1,143-byte transaction rather than on a rule.

The ceiling makes the property a rule instead:

| parameter | main / publictest | meaning |
| --- | --- | --- |
| `nTxPowMint` | 57,143 cinnabar | mint authorized per qualifying transaction |
| `nMaxBlockMint` | 2 × COIN | ceiling on total mint in one block |
| `nTailSubsidy` | 1 × COIN | perpetual base subsidy |

`nTxPowMint` is derived as `2 * COIN / N_tx,max` with `N_tx,max` ≈ 3,500
transactions per block, so a full block mints 2 coins — twice the perpetual tail
subsidy — and no block can ever mint more than that regardless of how the space
is filled.

## Out-of-Range Sums Are Rejected, Not Clamped

The running total is `MoneyRange`-guarded *before* the clamp. A sum that leaves
`MoneyRange` returns the `MINT_ALLOWANCE_INVALID` sentinel, and the block is
rejected as `bad-cb-mint-range`. It is not silently reduced to `nMaxBlockMint`.

The distinction matters: an out-of-range sum means a malformed block, and
clamping it would convert a consensus failure into a quietly accepted block. The
sentinel is negative, so it is itself outside `MoneyRange` and forces any caller
that forgets to test for it into rejecting the block anyway.

A coinbase above the resulting ceiling is rejected as `bad-cb-amount`.

## Replay Safety

Transaction PoW is part of the transaction id and binds to the transaction's
inputs, outputs, lock time, anchor, and nonce. A proof cannot be transplanted to a
different transaction without failing verification.

A transaction id can appear only once in a valid block and can be confirmed only
once on the active chain. That UTXO uniqueness gives Frame-B one mint per
confirmed, qualifying transaction without a separate spent-proof database.

## Mint-Safety Floor Binding

A mint is authorized for a transaction if and only if that transaction clears its
own per-transaction congestion target — the same `GetTxPowTarget` the validity
predicate uses. A proof that fails its congestion floor mints nothing, so a miner
cannot manufacture cheap self-transactions to harvest free mints once the floor
rises.

## Reorg Behavior

The mint allowance is a pure function of block contents and consensus
parameters. Disconnecting a block rolls back its coinbase through normal undo
logic; there is no separate mint state to maintain.

## Worked Example

From the publictest chain at height 106, which carried one transaction. All
amounts in cinnabar:

```
subsidy(106)   = 5000000000 - floor(4900000000 * 106 / 1051920)
               = 5000000000 - 493763
               = 4999506237                      # 49.99506237 Hg
mint           = min(1 * 57143, 200000000)
               = 57143                           # 0.00057143 Hg
coinbase limit = 4999506237 + 57143
               = 4999563380                      # 49.99563380 Hg
```

The block's coinbase paid exactly 4999563380. Its neighbours at heights 105 and
107 carried no transactions and paid 4999510895 and 4999501579 — pure subsidy on
a ramp that loses about 4,658 cinnabar per block. The mint is therefore visible
only by differencing against the subsidy curve; at the bootstrap subsidy a single
transaction moves the coinbase by roughly one part in 87,000.

The sender paid nothing. Transaction value is preserved exactly (`in == out`),
so the miner's compensation is new issuance authorized by the work the *sender*
already performed, not a transfer from the sender.

At tail emission the relationship inverts: base subsidy is 1 coin, a full block
mints 2, and transaction mints become the dominant component of miner income.

## Safety Rules

- A missing, stale, or tampered transaction proof authorizes no mint.
- A proof that fails its own congestion floor authorizes no mint.
- The mint amount per transaction is capped by `nTxPowMint`.
- The mint total per block is capped by `nMaxBlockMint`, independent of
  transaction size or count.
- An allowance sum outside `MoneyRange` rejects the block (`bad-cb-mint-range`);
  it is not clamped.
- A miner may claim less than the allowance.
- A miner claiming more than the allowance is rejected as an excessive coinbase
  (`bad-cb-amount`).

## Where This Is Enforced And Tested

| concern | location |
| --- | --- |
| allowance arithmetic | `src/pow.cpp` `GetBlockMintAllowance` |
| coinbase ceiling | `src/validation.cpp` `ConnectBlock` |
| assembler claim | `src/node/miner.cpp` |
| supply accounting | `src/index/coinstatsindex.cpp` |
| unit tests | `src/test/mint_tests.cpp` |
| external mining interface | `doc/mining/external-mining.md` (`mintvalue`) |
