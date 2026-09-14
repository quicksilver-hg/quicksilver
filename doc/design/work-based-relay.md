# Work-Based Relay

Quicksilver relay and mining policy use transaction work rather than monetary
fees. Consensus sets the required work floor; policy uses surplus work to decide
ordering, eviction, and replacement.

## Ranking

For a valid transaction:

```
actual_txwork = 2^256 / (CuckatooProofHash(nCycle) + 1)
floor         = RequiredTxWork(anchor)
surplus       = max(0, actual_txwork - floor)
rank_key      = surplus / vsize
```

The same `rank_key` is used from both ends of the relay pool:

- mining reads the highest surplus-work rate entries first;
- memory-pressure eviction drops the lowest surplus-work rate entries first.

This keeps mining and eviction aligned: a transaction the node would prefer to
mine is not the first transaction it evicts.

## Replacement

Replacement is the feeless equivalent of bumping a stuck transaction. A vault
can re-mine the same spend with a rarer proof. The replacement conflicts with the
old transaction and is accepted only when it carries strictly better work.

A replacement must:

- beat every direct conflict by surplus-work rate;
- exceed the total surplus work of the replaced direct conflicts and descendants;
- keep the work-agnostic bounds that prevent unbounded replacement chains.

Package replacement is intentionally unsupported because package aggregation
would let a child's rare proof lift unrelated low-work ancestors. Quicksilver
replacement is per transaction.

## Relay Floors

There is no monetary relay floor, rolling minimum fee, or fee filter. Congestion
pressure belongs in the transaction PoW requirement and surplus-work ranking.
The relay pool size limit remains an ordinary resource bound.

The BIP133 `feefilter` command is not a registered protocol message. BIP324
keeps short-id 5 as an unused slot so later assigned ids stay put. An inbound
message of that name is unknown traffic, not a known command this node ignores.
