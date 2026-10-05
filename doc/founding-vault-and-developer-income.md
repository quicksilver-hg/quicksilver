# Founding vault and developer income

The project's own mining pays two addresses. This page says which addresses they
are, what they hold, why, and how to check that on your own node.

| | Address | Paid by |
|---|---|---|
| Founding vault | `QUPztCykMU4tbcLANCWonrtSUHk2awCazq` | the project's four fleet miners |
| Developer income | `hg1qcdelxxgrsjdjvjwj79vkg22a2cj5fgna7r24u4` | one developer machine, from after height 10000 |

## Reading

<!-- RESERVED. The only place on this page for figures from a reading. Fill it in the change made after height 10000, before the developer machine mines: height 10000, the UTC time of block 10000, and for the founding vault the amount in Hg and the number of coinbase outputs from heights 1 through 10000. Add the height of the first block that pays the developer income address when it exists. No other line on the page carries a number from a reading. -->

## What the founding vault holds

The founding vault holds the block rewards from the project's fleet: four
machines, all paying this one address. Every coinbase the chain paid from height
1 until the developer machine started mining pays this address, and none of
those outputs has been spent.

Height 0 is the genesis coinbase. Its output script is `OP_RETURN`, and the node
leaves that transaction out of the UTXO set. The outputs start at height 1.

The project sends nothing to the founding vault and spends nothing from it until
a decision is made about its coins. If it ever spends, the transactions will be
recorded on this page. Anyone can send coins to an address; an unspent output at
the founding vault that is not a coinbase was sent by someone else.

## What the developer income address holds

The developer income address is paid by one developer machine. It starts mining
after height 10000. Which block it wins first is left to chance; that height
will be added to this page. It receives no coins from the fleet, and the
founding vault receives none from it.

Its coins are used to test transfers on the live network, including transfers
back to itself. Those spends are not recorded on this page.

## Why they hold them

The coins in both addresses are block rewards from the project's own mining. A
block's coinbase names the address it paid, and that stays in the block after
the coins are spent.

## What has not been decided

No decision has been made about the founding vault's coins. This page records
what each address holds and why the coins were mined. When a decision is made,
it will be recorded on this page with the transactions that carry it out.

## How to check this yourself

Use a mainnet node. Pass `-chain=main` on each command. That is the default
chain, and naming it keeps the commands on mainnet.

**The founding vault.**

```
quicksilver-cli -chain=main scantxoutset start '["addr(QUPztCykMU4tbcLANCWonrtSUHk2awCazq)"]'
```

`success` is true. Each entry in `unspents` has a `height`, the block that
created it, and `coinbase`. The entries with `"coinbase": true` are the fleet's
block rewards, and none has been spent: no coinbase that paid this address is
missing from the scan. Entries with `"coinbase": false`, if any, were sent by
someone else. `txouts` on this command is how many outputs were scanned, not how
many matched.

**Any block.** A coinbase names the address it paid, whether or not its coins
have been spent since. Pick a height from 1 through your node's tip:

```
quicksilver-cli -chain=main getblockhash <height>
quicksilver-cli -chain=main getblock <hash> 2
```

`tx[0]` is the coinbase. `vin[0]` contains `coinbase`. Its `vout` entry has
`output_script.address` set to the address it paid. From height 1 through 10000
that is the founding vault. After 10000, a block the developer machine mined
pays the developer income address, and a block from the fleet still pays the
founding vault.

**The developer income address.** Its coinbase rewards are found in blocks as
above. A scan shows only what it holds now, which changes as it transfers:

```
quicksilver-cli -chain=main scantxoutset start '["addr(hg1qcdelxxgrsjdjvjwj79vkg22a2cj5fgna7r24u4)"]'
```

**Height 0.**

```
quicksilver-cli -chain=main getblockhash 0
quicksilver-cli -chain=main getblock <hash> 2
```

The coinbase `vout` `output_script.asm` contains `OP_RETURN`. That output is
absent from the scans.
