# Founding vault and developer vault

The project's own mining has paid a small set of addresses. This page says which
addresses they are, what they hold, why, what changed at height 20000, and
how to check every statement on your own node.

| | Address(es) | Paid by |
|---|---|---|
| Founding vault | `QUPztCykMU4tbcLANCWonrtSUHk2awCazq` | the project's miners |
| Developer vault | six addresses of one vault, listed [below](#the-developer-vault-addresses) | one developer machine, from height 10001 through 20000 |

## Reading

Taken at height 20100, block
`09f47371cd826271341be70dc892991818dc21db92a2006925e77c1c20f6a082`
(2026-11-24 01:33:34 UTC), on one node with the commands in
[How to check this yourself](#how-to-check-this-yourself).

| | Blocks | Amount |
|---|---|---|
| Founding vault, heights 1 to 19999 | 16276 | 807,213.69693768 Hg |
| Developer vault, six addresses, heights 10001 to 20000 | 3723 | 183,420.06381166 Hg |
| Block 10973, project test machine | 1 | 49.48886133 Hg |
| **Heights 1 to 20000** | **20000** | **990,683.24961067 Hg** |
| Heights 20001 to 20100, no reward claimed | 100 | 0 Hg |

Every block from height 1 to 20000 pays one of the eight addresses on this
page. The total equals every coin that exists at height 20100
(`gettxoutsetinfo` `total_amount`).

None of the founding vault's 16276 coinbase outputs has been spent. The last
reward claimed, block 20000, has 101 confirmations at height 20100, so every
reward in the table has matured.

The 100 blocks from height 20001 to 20100 claimed nothing. The rewards they
did not claim, 4,906.6017858 Hg, were never created.

## What the founding vault holds

The founding vault holds block rewards from the project's miners. Every block
from height 1 to height 10000 pays this address, and so do the project's
miners' later blocks through height 19999. None of its coinbase outputs has
been spent, and no other transaction has paid it.

Height 0 is the genesis coinbase. Its output script is `OP_RETURN`, and the node
leaves that transaction out of the UTXO set. The outputs start at height 1.

## The developer vault addresses

One developer machine mined for the project from height 10001 through 20000. It
paid an address of one vault, and over time that vault has used six addresses:

| Address | First block | Last block | Blocks |
|---|---|---|---|
| `hg1qdjtxfzy3ldneztgupvyrugmf54ug44z9az8jwr` | 10001 | 10002 | 2 |
| `hg1qma5c00leh7eatmnq007nyxpq2sk5n94gpc7zmg` | 10008 | 10183 | 56 |
| `hg1qu52pmvvgzc3ggq542qgc7ynz5elulw0sq7fden` | 10193 | 10805 | 178 |
| `hg1qe0trtsrj0tyjg5jls4txr5zhk4pahw524429pu` | 10821 | 10947 | 46 |
| `hg1q8v5smqgmqzsrux07r6sh3cvegljtez3wuxf6gj` | 10959 | 11938 | 368 |
| `hg1q6r986g74menuhe0r8s930m4suny4lalzrt6jzc` | 13532 | 20000 | 3073 |

An earlier version of this page named
`hg1qcdelxxgrsjdjvjwj79vkg22a2cj5fgna7r24u4` as the developer income address.
No block and no transaction has ever paid it. The developer machine's mining
paid the addresses above instead.

The developer vault also holds ordinary change and received coins. Its coins
are used to test transfers on the live network.

The developer vault also received the reward of block 10973. That block paid
`hg1qrchw29zg7max78pqjrzspdwwlq7pvyklfef8l8`, an address of a project test
machine. The reward was spent into the developer vault as one of the six inputs
of transaction
`7737ae26aa52223d67ed8bc8cd86384640d7b5a323a964adbb78524ee64b74d5`, at height
14567.

The developer vault sent coins to vaults on other project machines to test
transfers. All of these coins were then returned to the developer vault.

## What changed at height 20000

Height 20000 was the last block on which the project's miners claimed the
block reward. From height 20001 their blocks pay nothing: the coinbase output
is an unspendable `OP_RETURN` script with value 0, using the node's
`-minezeroclaim` mode. Those blocks still carry and confirm transactions.
Coins from a reward that is not claimed are never created.

This stops the project's holdings from growing.

While only the project mines, every block that claims a reward is the
project's, so both halves can be checked on any node: the fixed amount kept,
and the growing amount refused. Once other people mine, a block that claims a
reward could be anyone's, and the chain cannot show who mined it. From then on
the project's zero-claim mining is a commitment it states, not something a
reader can verify.

## What has been decided about the founding vault

The founding vault's coins are intended to be distributed, not kept as a
reserve.

## How to check this yourself

Use a mainnet node. Pass `-chain=main` on each command. That is the default
chain, and naming it keeps the commands on mainnet.

**The founding vault.**

```
quicksilver-cli -chain=main scantxoutset start '["addr(QUPztCykMU4tbcLANCWonrtSUHk2awCazq)"]'
```

`success` is true. Each entry in `unspents` has a `height`, the block that
created it, and `coinbase`. All of them are `"coinbase": true`, and none has
been spent. `txouts` on this command is how many outputs were scanned, not how
many matched.

**Any block.** A coinbase names the address it paid. Pick a height from 1
through your node's tip:

```
quicksilver-cli -chain=main getblockhash <height>
quicksilver-cli -chain=main getblock <hash> 2
```

`tx[0]` is the coinbase. Its first `vout` entry has `output_script.address` set
to the address it paid. From height 1 through 10000 that is the founding vault.
From 10001 through 20000, a block the developer machine mined pays one of the
six addresses above, block 10973 pays the test address, and every other block
pays the founding vault. From 20001, a block the project's miners mine has a
first `vout` with `value` 0, `output_script.asm` `OP_RETURN` and no address.

**The developer vault addresses.** Their coinbase rewards are found in blocks
as above. A scan shows only what an address holds now, which changes as the
vault transfers:

```
quicksilver-cli -chain=main scantxoutset start '["addr(hg1q6r986g74menuhe0r8s930m4suny4lalzrt6jzc)"]'
```

**That nobody else mined.** List the payee of every coinbase. This needs `jq`
and takes a few minutes:

```
tip=$(quicksilver-cli -chain=main getblockcount)
for h in $(seq 1 "$tip"); do
  quicksilver-cli -chain=main getblock "$(quicksilver-cli -chain=main getblockhash "$h")" 2 |
    jq -r '.tx[0].vout[0] | (.output_script.address // .output_script.asm) + " " + (.value|tostring)'
done | awk '{ n[$1]++; if ($2 + 0 > 0) paid[$1]++ } END { for (a in n) print n[a], paid[a] + 0, a }' | sort -rn
```

Each line is a block count, how many of those blocks paid a reward, and the
payee. Through height 20000 the payees are the eight addresses on this page,
with the counts in the Reading. `OP_RETURN` lines are blocks that claimed
nothing.

**The rewards not claimed.** A node started with `-coinstatsindex` keeps a
running total of them:

```
quicksilver-cli -chain=main gettxoutsetinfo muhash <height>
```

`total_unspendable_amount` is 50 Hg (the genesis coinbase) plus every reward
not claimed up to that height. `block_info.unspendables.unclaimed_rewards` is
the reward that block itself did not claim.

**Height 0.**

```
quicksilver-cli -chain=main getblockhash 0
quicksilver-cli -chain=main getblock <hash> 2
```

The coinbase `vout` `output_script.asm` contains `OP_RETURN`. That output is
absent from the scans.
