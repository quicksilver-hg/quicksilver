# Fee-Plumbing Purge

The fee-plumbing purge completed Quicksilver's hard removal of live monetary
transaction-fee surfaces.

## Outcome

The cleanup removed remaining vault, validation, relay pool, package, mining, and
functional-test plumbing that only existed to carry zero transaction-fee values.
It also strengthened the static residue lint so future changes cannot re-add
removed fee fields, prioritization RPCs, or vault compatibility options.

The BIP133 `feefilter` command is not a registered protocol message. BIP324
short-id 5 is an unused slot so later assigned ids stay put. Inbound traffic of
that name is unknown, not a known command counted under `feefilter`.

## Preserved Behavior

The purge did not change Quicksilver's feeless consensus rule:

- non-coinbase transactions must preserve value exactly;
- non-feeless transactions still reject as `bad-txns-not-feeless`;
- relay pool admission and mining priority remain work-based;
- coinbase validation remains subsidy plus mint allowance.

## Verification

The verification pass built the full tree, ran the default non-GUI unit test
binary, ran the fee-residue lint and its self-test, ran the branding lint, and
used targeted searches for the removed fee-plumbing tokens.

The remaining differences against older fee-removal work were classified as
intentional wins from the current tree, such as removed vault-backend paths
and work-fraction naming.
