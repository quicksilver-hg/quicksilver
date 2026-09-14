# Relay Pool Limits

## Definitions

Given any two transactions Tx0 and Tx1 where Tx1 spends an output of Tx0,
Tx0 is a *parent* of Tx1 and Tx1 is a *child* of Tx0.

A transaction's *ancestors* include, recursively, its parents, the parents of its parents, etc.
A transaction's *descendants* include, recursively, its children, the children of its children, etc.

A relay pool entry's *ancestor count* is the total number of unconfirmed transactions in the relay
pool in its ancestor set, including itself.
A relay pool entry's *descendant count* is the total number of unconfirmed transactions in the relay
pool in its descendant set, including itself.

A relay pool entry's *ancestor size* is the aggregated virtual size of the unconfirmed transactions
in the relay pool in its ancestor set, including itself.
A relay pool entry's *descendant size* is the aggregated virtual size of the unconfirmed
transactions in the relay pool in its descendant set, including itself.

Transactions submitted to the relay pool must not exceed the ancestor and descendant limits (aka
relay pool *package limits*) set by the node (see `-limitancestorcount`, `-limitancestorsize`,
`-limitdescendantcount`, `-limitdescendantsize`).

## Exemptions

### Child Carve Out

**Child Carve Out** if a transaction candidate for submission to the
relay pool would cause some relay pool entry to exceed its descendant limits, an exemption is made
if all of the following conditions are met:

1. The candidate transaction is no more than 10,000 virtual bytes.

2. The candidate transaction has an ancestor count of 2 (itself and exactly 1 ancestor).

3. The descendant count of the transaction already in the relay pool, including the candidate
   transaction, would only exceed the limit by 1.

*Rationale*: this rule was introduced to prevent pinning by domination of a transaction's descendant
limits in two-party contract protocols such as LN.

### Single-Conflict Replacement Carve Out

When a candidate transaction for submission to the relay pool would replace relay pool entries, it
may also decrease the descendant count of other relay pool entries. Since ancestor/descendant limits
are calculated prior to removing the would-be-replaced transactions, they may be overestimated.

An exemption is given for a candidate transaction that would replace transactions in the relay pool
and meets all of the following conditions:

1. The candidate transaction has exactly 1 directly conflicting transaction.

2. The candidate transaction does not spend any unconfirmed inputs that are not also spent by the
   directly conflicting transaction.

The following discounts are given to account for the would-be-replaced transaction(s):

1. The descendant count limit is temporarily increased by 1.

2. The descendant size limit temporarily is increased by the virtual size of the to-be-replaced
   directly conflicting transaction.
