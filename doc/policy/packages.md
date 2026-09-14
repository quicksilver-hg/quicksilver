# Package Relay Pool Accept

## Definitions

A **package** is an ordered list of transactions representable by a connected
Directed Acyclic Graph. A directed edge exists between a transaction that spends
the output of another transaction.

For every transaction `t` in a **topologically sorted** package, if any of its
parents are present in the package, they appear somewhere in the list before
`t`.

A **child-with-unconfirmed-parents** package is a topologically sorted package
that consists of exactly one child and all of its unconfirmed parents. No other
transactions may be present. The last transaction in the package is the child,
and each input must be available in the UTXO set as of the current chain tip or
some preceding transaction in the package.

## Package Relay Pool Acceptance Rules

The following rules are enforced for all packages:

* Packages cannot exceed `MAX_PACKAGE_COUNT=25` count and
  `MAX_PACKAGE_WEIGHT=404000` total weight.

  - *Rationale*: Package validation should remain small enough to limit DoS
    surface while still allowing ancestor packages that would be valid if
    submitted individually.

  - Note that, if relay pool limits change, package limits should be reconsidered.
    Users may also configure their relay pool limits differently.

  - Note that this is transaction weight, not "virtual" size as with other
    limits, to allow simpler context-less checks.

* Packages must be topologically sorted.

* Packages cannot have conflicting transactions. No two transactions in a
  package can spend the same inputs, and packages cannot contain duplicate
  transactions.

* When packages are evaluated against ancestor and descendant limits, the union
  of all transactions' descendants and ancestors is considered.

  - *Rationale*: This is a worst-case heuristic for packages that are heavily
    connected.

* [Child Carve Out](./relaypool-limits.md#child-carve-out) is disabled in packaged
  contexts.

  - *Rationale*: This carve out cannot be accurately applied when multiple
    transactions' ancestors and descendants are considered at the same time.

The following rules are only enforced for packages submitted to the relay pool, not
for test accepts:

* Packages must be child-with-unconfirmed-parents packages. This also means
  packages must contain at least one transaction.

  - *Rationale*: Restricting packages to a defined topology is easier to reason
    about and simplifies validation.

* Transactions in the package that have the same txid as another transaction
  already in the relay pool are removed from the package before submission
  ("deduplication").

  - *Rationale*: Nodes may receive transactions in different orders. There is no
    need to repeat validation for transactions already in the relay pool.

## Package Replacement and Value Semantics

Quicksilver has no transaction-fee field or fee market. Package acceptance and
replacement policy do not compare monetary fees. Transactions must preserve
value exactly, and block inclusion uses per-transaction proof-of-work surplus
where ordering is required.

Quicksilver does not support package-level replacement. If any transaction in a
package would replace a relay pool transaction, package validation rejects the
package with `package-replacement-unsupported-feeless`.

Replacement is intentionally per-transaction and keyed to surplus work. Package
replacement would aggregate parent and child work into one ranking decision,
which is not rank-neutral under Quicksilver's feeless per-tx-work policy.

## Feeless Package Semantics

Quicksilver transactions must be feeless: input value equals output value.
Package validation does not let one transaction pay for another transaction, and
there is no package-level payment or relay floor.

Packages are still useful for submitting a child with its unconfirmed parents in
one RPC call. Each transaction must individually satisfy consensus and the
current relay pool policy, including its own per-transaction proof-of-work and
surplus-work ranking where applicable.
