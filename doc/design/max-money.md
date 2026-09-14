# MAX_MONEY

`MAX_MONEY` is a technical per-value overflow guard in Quicksilver. It is not an
economic cap on cumulative supply.

Quicksilver has a perpetual tail subsidy and Frame-B minting, so total supply is
designed to grow without a fixed terminal cap. A 21-million-style cumulative cap
is not part of Quicksilver's monetary policy.

## Overflow Guard

`MoneyRange(value)` still rejects negative amounts and values above `MAX_MONEY`.
That protects arithmetic sites that sum transaction outputs or validate coinbase
amounts.

The guard is intentionally much lower than `INT64_MAX` so common incremental
sum patterns cannot overflow a signed 64-bit `CAmount` before a range check runs.

## Consensus Meaning

Raising or lowering `MAX_MONEY` changes which individual output amounts are
valid, so it is a consensus parameter. It should be treated as a value-domain
limit, not as a supply schedule.

The base subsidy schedule and Frame-B mint rules determine issuance. `MAX_MONEY`
only bounds individual values.

## Audit Notes

The high ceiling depends on Quicksilver's feeless invariant. If a future design
ever reintroduces non-zero transaction fees or large value accumulation outside
the existing per-transaction output-sum pattern, the overflow audit must be
reopened.
