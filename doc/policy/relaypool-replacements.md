# Relay Pool Replacements

## Current Replace-by-Surplus-Work Policy

A transaction conflicts with a transaction in the relay pool if they spend one or
more of the same inputs. A replacement transaction may replace its directly
conflicting transactions and their descendants in the relay pool if it passes all
other consensus and policy rules and satisfies the replacement checks below.

Quicksilver has no transaction-fee field or fee market. Package acceptance and
replacement policy do not compare monetary fees. Transactions must preserve
value exactly, and block inclusion uses per-transaction proof-of-work surplus
where ordering is required. The same surplus-work rate used for relay pool eviction
and mining order is used to decide whether a replacement is preferable to each
direct conflict.

1. The replacement transaction may only include an unconfirmed input if that
   input was included in one of the directly conflicting transactions.

   *Rationale*: This keeps replacement from pulling in new unconfirmed
   dependencies that were not part of the original conflict set.

2. The number of original transactions must not exceed the replacement
   candidate limit. More precisely, the sum of all directly conflicting
   transactions' descendant counts, inclusive of the conflicting transactions
   themselves, must not exceed the configured limit.

   *Rationale*: This limits how much of the relay pool one replacement can evict.

3. The replacement transaction's surplus-work rate must be greater than the
   surplus-work rates of all directly conflicting transactions.

   *Rationale*: Replacement and block assembly use the same ranking primitive.

4. The replacement transaction's absolute surplus work must be greater than the
   sum of every replaced transaction's surplus work.

   *Rationale*: A high-rate but low-total-work transaction must not cheaply evict
   several high-surplus conflicts. There is no relay bandwidth term; each
   higher surplus proof represents an actual Cuckatoo solve.

This policy is similar in shape to inherited BIP125 replacement, but it is
replace-by-surplus-work. Opt-in signaling is not the economic primitive in
Quicksilver's relay pool policy.
