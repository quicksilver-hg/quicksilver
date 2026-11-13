# Founding vault and developer vault

The project's own mining has paid a small set of addresses. This page says which
addresses they are, what they hold, why, what will change at height 20000, and
how to check every statement on your own node.

| | Address(es) | Paid by |
|---|---|---|
| Founding vault | `QUPztCykMU4tbcLANCWonrtSUHk2awCazq` | the project's miners |
| Developer vault | six addresses of one vault, listed [below](#the-developer-vault-addresses) | one developer machine, from height 10001 |

## Reading

The figures for this page will be added once block 20000, the last block on
which the project's miners will claim a reward, has matured: at height 20100 or
later, since a coinbase output can be spent only after 100 further blocks. They
will be taken from one node with the commands in
[How to check this yourself](#how-to-check-this-yourself), and give the block
count, the founding vault's coinbase outputs and amount, and the developer
vault blocks per address.

## What the founding vault holds

The founding vault holds block rewards from the project's miners. Every block
from height 1 to height 10000 pays this address, and so do the project's
miners' blocks after it. None of its coinbase outputs has been spent, and no
other transaction has paid it.

Height 0 is the genesis coinbase. Its output script is `OP_RETURN`, and the node
leaves that transaction out of the UTXO set. The outputs start at height 1.

## The developer vault addresses

One developer machine mines for the project from height 10001. It pays an
address of one vault, and over time that vault has used six addresses:

| Address | First block | Last block | Blocks |
|---|---|---|---|
| `hg1qdjtxfzy3ldneztgupvyrugmf54ug44z9az8jwr` | 10001 | 10002 | 2 |
| `hg1qma5c00leh7eatmnq007nyxpq2sk5n94gpc7zmg` | 10008 | 10183 | 56 |
| `hg1qu52pmvvgzc3ggq542qgc7ynz5elulw0sq7fden` | 10193 | 10805 | 178 |
| `hg1qe0trtsrj0tyjg5jls4txr5zhk4pahw524429pu` | 10821 | 10947 | 46 |
| `hg1q8v5smqgmqzsrux07r6sh3cvegljtez3wuxf6gj` | 10959 | 11938 | 368 |
| `hg1q6r986g74menuhe0r8s930m4suny4lalzrt6jzc` | 13532 | still paid | 1752 to height 17204 |

The Reading will carry the final count for the last address.

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

## What will change at height 20000

Height 20000 will be the last block on which the project's miners claim the
block reward. From height 20001 their blocks will pay nothing: the coinbase
output will be an unspendable `OP_RETURN` script with value 0, using the node's
`-minezeroclaim` mode. Those blocks will still carry and confirm transactions.
Coins from a reward that is not claimed are never created.

This stops the project's holdings from growing.

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
From 10001, a block the developer machine mined pays one of the six addresses
above, block 10973 pays the test address, and every other block pays the
founding vault. From 20001, a block the project's miners mine will have a first
`vout` with `value` 0, `output_script.asm` `OP_RETURN` and no address.

**The developer vault addresses.** Their coinbase rewards are found in blocks
as above. A scan shows only what an address holds now, which changes as the
vault transfers:

```
quicksilver-cli -chain=main scantxoutset start '["addr(hg1q6r986g74menuhe0r8s930m4suny4lalzrt6jzc)"]'
```

**That nobody else mined.** Walk the blocks and list the address in each
coinbase. The set is the eight addresses on this page that were paid: the
founding vault, the six developer vault addresses and the block-10973 test
address.

**Height 0.**

```
quicksilver-cli -chain=main getblockhash 0
quicksilver-cli -chain=main getblock <hash> 2
```

The coinbase `vout` `output_script.asm` contains `OP_RETURN`. That output is
absent from the scans.
