# Feeless Transactions

Quicksilver has no monetary transaction fee concept. A normal transaction is
valid only when its inputs and outputs preserve value exactly.

## Consensus Rule

For every non-coinbase transaction:

```
input value == output value
```

If inputs are below outputs, the transaction creates value and is invalid. If
inputs exceed outputs, the transaction destroys value and is invalid as
`bad-txns-not-feeless`.

The coinbase reward does not include a fee term. It is limited to the base
subsidy plus Frame-B mint allowance.

## Vault Construction

Vault-created transactions select inputs to preserve value exactly. When an
exact input match exists, the vault can avoid change. Otherwise it emits a
change output for the full leftover amount.

Small outputs are not treated as fee dust. Spam resistance comes from transaction
PoW and block capacity, not from making small outputs uneconomic to spend.

## Public Interfaces

Quicksilver-owned RPCs and docs should not expose zero-valued transaction fee
fields as compatibility shims. Public outputs should instead expose current
Quicksilver concepts where relevant:

- transaction work;
- surplus work and work rate;
- subsidy;
- mint value;
- exact input and output values.

OP_RETURN burn limits remain a separate explicit-output policy and are not
transaction fees.

The BIP133 `feefilter` command is not registered. BIP324 short-id 5 is an unused
slot so later assigned ids stay put.
