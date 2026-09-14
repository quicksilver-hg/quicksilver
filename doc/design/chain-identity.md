# Chain Identity

Quicksilver is a fresh chain with its own network identity, genesis blocks, ports,
address prefixes, and trust anchors. It does not share network magic, checkpoints,
seed lists, chain work, or transaction history with any inherited network. There
is no UTXO-snapshot bootstrap in v1.

## Networks

Each Quicksilver network has distinct message-start bytes, default ports, address
prefixes, and genesis hash. Sandbox remains the local development network.

Public chain parameters are defined in [chainparams.cpp](../../src/kernel/chainparams.cpp).
Base port and network-name plumbing lives in
[chainparamsbase.cpp](../../src/chainparamsbase.cpp).

## Address Identity

Quicksilver keeps the proven script and UTXO model while using Quicksilver address
presentation. Bech32 human-readable prefixes identify Quicksilver addresses on
each network, and inherited address prefixes are not valid Quicksilver public
identity.

## Genesis

Genesis is a hardcoded trust anchor. The genesis hash and merkle root are asserted
in chain parameters, and the genesis block carries an all-zero Cuckatoo cycle.
Normal Cuckatoo block PoW begins after genesis.

The genesis coinbase is unspendable, as in the inherited UTXO model. Current
Quicksilver genesis outputs use a provably unspendable project mark instead of a
spendable key.

## Trust Anchors

Fresh-chain parameters intentionally reset inherited trust anchors:

- no checkpoints;
- no DNS seeds, and an onion fixed seed on `main` and `publictest` only;
- zero minimum chain work;
- zero default assumed-valid block;
- no UTXO-snapshot bootstrap;
- chain transaction metadata reset for the fresh ledger.

## Why BIP numbers appear in this source tree

Quicksilver keeps its BIP citations. A BIP number is a citation to a public
technical standard this code genuinely implements — BIP32 derivation, BIP143
sighash, BIP158 filters, BIP341 taproot, BIP324 transport. Removing the number
does not remove the inheritance; it removes the reader's ability to check it,
and replaces an accurate reference with an unsourced assertion. That is the
opposite of the honesty standard applied everywhere else here.

This is a different question from the residue that *was* purged. `PSBT`, the
old testnet names and the fee plumbing were upstream's *choices*, carried
without reason. A BIP number is a *source*.

Keeping the citations creates an obligation that each one is true. Two classes
of citation are not, and were corrected rather than kept:

- **A citation for behaviour Quicksilver does not have.** BIP125 describes
  a fee-based replacement policy. Quicksilver replaces by surplus work.
  An earlier pass kept the
  citations that named BIP125's *signalling* convention — the `nSequence` opt-in,
  `MAX_BIP125_RBF_SEQUENCE`, `SignalsOptInRBF` — on the grounds that they
  described something the node actually did, and removed only the citations that
  named the *policy*. That distinction did not survive inspection: `RelayPoolAccept`
  sets replacement eligibility from conflicts alone, so nothing ever read the
  signal. The signalling apparatus was dead code wearing an accurate citation, and
  both went together. What remains are the "Rule 3/6 analog" comments in
  `policy/replacement.h`, which cite BIP125's *rules* as the lineage of a work-denominated
  test that really is descended from them.
  BIP133 describes a fee-rate filter on transaction relay. An earlier pass kept
  the command registered so BIP324 short ids would stay put, and ignored inbound
  copies. That is the same mistake: a fee-named protocol message wearing a
  transport citation. The type is not registered. BIP324 short-id 5 is unused so
  later assigned ids stay put — the numbering is the BIP324 citation; the
  fee-named command is not.
- **A citation on an external surface.** The `bip125-replaceable` JSON field on
  `getrelaypoolentry` and `gettransaction` was renamed to `replaceable`, and the
  `bip125-replacement-disallowed` reject token to `replacement-disallowed`. An
  API field name is a contract, not a comment, and this one asserted fee-based
  replacement on a feeless chain. The reject token stands. The `replaceable`
  fields did not survive the purge above: they were computed from the dead signal,
  so they reported `false` for transactions this node will replace. On a chain
  where every unconfirmed transaction is replaceable by more work, the field
  carried no information, and it was removed rather than made truthful — along
  with `getrelaypoolinfo`'s `fullrbf`, whose `(DEPRECATED)` marker v1 does not
  ship.
