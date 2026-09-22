# Agent Client

> Status: core implementation complete. Header storage/synchronization, P2P
> payload adapters, peer routing, the command-line transport harness, and the
> client-side agent allotment policy-request decoder plus daily-guardrail check are
> in tree and covered by focused tests. The command-line agent can check a
> desktop-issued policy request against a proposed spend amount. The desktop
> vault can export a funded setup as a policy-plus-funding-key bundle for the
> gateway handoff, and the command-line agent can verify that bundle's funding
> key matches its funding address before applying the same guardrail check.
> Agent-side key import, bundle-output spend signing, persisted payment receipt
> ingestion, local receipt-inbox scanning, output-level receipt activity history,
> optional receipt metadata, post-spend change receipt rotation, a persistent
> configured relay peer store, local-node addrman import, configured-peer header
> sync, and configured-peer address-gossip import are now wired through the
> command-line consumer. The desktop can now save a reviewed receipt artifact,
> the current funding receipts for a funded setup, or a reviewed signed-spend
> change receipt into that local agent inbox and scan it directly into the
> durable receipt store. It shows the refreshed spendable-output count and total,
> while the shared scanner prevents spent activity from being re-imported from
> retained inbox files. It still copies the matching `scanreceipts` command for
> external agent hosts. Manual configured-peer transaction relay is also wired
> through the command-line consumer, the desktop can save a policy bundle locally and
> copy a runnable `signbundle` command for a requested agent spend, and the
> desktop can copy a
> `scantxoutset`-to-`importrecovery` command for a recorded agent funding
> address. It can also import local-node peers directly into the agent relay
> peer store, copy a `getnodeaddresses` import pipeline for an external agent
> host, and run the bundle-output signing path in-process while merging and
> rotating persisted receipt-store entries before reviewing the resulting signed
> spend. The desktop can also run transaction relay in the background after
> review instead of only copying the `sendtxpeer` command. It attempts the typed
> peer, stored peers, or the active network's fixed seeds without blocking the
> GUI, and reports partial peer failures after all attempts finish.
> Public seed infrastructure now ships: `vFixedSeeds` carries an onion seed on
> `main` and `publictest`. Network-native payment discovery and full desktop vault
> integration remain deferred; see [v1-scope.md](v1-scope.md).

Quicksilver's intended users are software agents. A full node is not something an
agent can run: the chain grows by roughly 154 GB per year archival, or 26 GB per
year pruned (see [chain-storage.md](chain-storage.md)), which is a cost no
ordinary agent host will carry. The agent client exists so that an agent can hold
and spend quicksilver without one.

## Thin core

The client follows headers and nothing else.

A Quicksilver header is 252 bytes — an 84-byte pre-pow (the inherited 80-byte layout
plus the 4-byte congestion multiplier `nCongestion`) followed by a 42-element Cuckatoo
cycle of 4-byte entries, carried inside `CBlockHeader` in
[block.h](../../src/primitives/block.h). At the target block spacing this is about
26.5 MB per year, which is three orders of magnitude below the full chain and small
enough to be unremarkable on any host. `nCongestion` cost 1.6% of that budget
(0.42 MB/year) and bought the thin client the ability to prove per-transaction work at
all; the same 4 bytes are 0.0003% of an archival full node's annual growth.

`nCongestion` sits *before* `nNonce` in the pre-pow, so `nNonce` remains its trailing
four bytes. Every Cuckatoo solver grinds that tail in place, so the ordering is load-
bearing rather than cosmetic.

The client never scans blocks and stores no chain history. It speaks the ordinary
peer-to-peer protocol — `getheaders`/`headers` for the chain, `inv`/`tx` for
transactions — rather than REST or RPC, because those require a cooperating node
operator to have opted in.

Compact block filters (BIP157/158) are not used. At Quicksilver's block size they
would cost several gigabytes per year against 26.5 MB of headers, which defeats the
purpose of the client.

### The congestion multiplier, and what trusting it is worth

The per-transaction work target is `base * m / CONGESTION_ONE`. `base` comes from
`GetBlockProof`, which reads only `nBits`, so a header-only client can derive it. `m`
— the EIP-1559 congestion multiplier — is a function of connected block *weight*,
which is not in the header and is not reachable from a header chain.

Headers therefore commit `m` directly, as `nCongestion`. A full node does not read the
field as gospel: `ConnectBlock` recomputes the multiplier from the parent's value and
this block's weight and rejects any mismatch as `bad-congestion`. That check can only
run at connect time, because that is the first point at which the block's weight is
known — it is not decidable when a header is accepted.

For an agent this means `m` is **unvalidated, SPV-grade** data. Unlike `nBits`, which an
SPV client can re-derive from header timestamps, `nCongestion` cannot be checked from
headers alone. What the agent actually relies on is: *this header sits on the most-work
chain, and a full node would have rejected the block at connect if `m` were wrong.*

Both failure directions are bounded, and neither costs the agent funds:

- **Inflated `m`** — the agent over-proves. Wasted work; the transaction is still valid.
- **Deflated `m`** — the agent under-proves. The transaction is rejected. Annoying, not
  a loss.

This is a documented property of the design, not an unexamined assumption. The
alternative considered and rejected was over-proving at the `nCongestionMaxMultiplier`
cap: at a cap of 64 against `nTxWorkCouplingK = 106` that costs 60.4% of a full block's
work for a single transaction, and scales with network hashrate. That is a miner, not a
thin client.

`-prove=0` remains an offline construction/test mode and does not produce a relayable
transaction.

### Anchor age gives the client slack

Transaction proof-of-work commits to an anchor block, and `nMaxAnchorAge` allows
100 blocks — roughly 8.3 hours. A client that has been offline does not race the
tip to produce a valid transaction, so header synchronisation is not on the
critical path of a send.

## One core, two consumers

The core is an in-tree C++ library. The command-line agent client links it, and so
does the desktop application's vault.

This follows from the desktop design: the vault must also avoid the storage cost,
so it runs on the same thin core rather than on an embedded full node. A separate
binary invoked as a subprocess, or a reimplementation in another language, could
not serve both. One library means one serialization, one sighash, and one
176-byte transaction proof-of-work tail across everything that builds a
Quicksilver transaction.

## Implementation map

The implemented slice is intentionally transport-agnostic. It creates and
processes ordinary P2P message payloads, owns local header state, and queues
outbound messages for a caller-provided transport. Its allotment-spend primitive can
import explicitly shared key material, select known funding outputs, sign
transactions, and grind transaction proof-of-work, but the core still does not
own a rich durable allotment or persist payment history beyond the command-line
receipt store and its output-level activity ledger. The command-line consumer
provides the current manual socket transport harness and configured-peer
discovery harness.

- **Header state:** [headerchain.h](../../src/agent/headerchain.h) validates and
  stores a header tree from genesis, following the most-work tip (first-seen
  wins on equal work). It stores `CBlockIndex` metadata only, not blocks or
  transaction history. A `getheaders` response that starts at a common ancestor
  rather than the current tip is accepted; a two-block side chain that overtakes
  becomes the new tip.
- **Header persistence:** [headerstore.h](../../src/agent/headerstore.h) saves and
  reloads the local header chain with explicit framing, chain genesis checking,
  and invalid-header reporting. The vault-first desktop watches this same
  network-scoped store and applies its verified tip to detached vaults for
  balance refresh and confirmation depth; an attached consensus chain remains
  authoritative.
- **Header sync:** [headersync.h](../../src/agent/headersync.h),
  [headermessages.h](../../src/agent/headermessages.h),
  [headerdriver.h](../../src/agent/headerdriver.h), and
  [headerpeer.h](../../src/agent/headerpeer.h) implement `getheaders`/`headers`
  payload construction, decoding, batching, duplicate handling, and per-peer
  outbound queues.
- **Transaction relay payloads:** [txmessages.h](../../src/agent/txmessages.h) and
  [txpeer.h](../../src/agent/txpeer.h) implement transaction `inv`, `getdata`, and
  `tx` payload construction/decoding. Serialized transactions preserve witness
  data and the Quicksilver transaction proof-of-work tail.
- **Client facade:** [agentclient.h](../../src/agent/agentclient.h),
  [agentpeer.h](../../src/agent/agentpeer.h), and
  [agentpeerset.h](../../src/agent/agentpeerset.h) combine header and transaction
  state behind default-peer and multi-peer APIs.
- **Allotment policy:** [allotmentpolicy.h](../../src/agent/allotmentpolicy.h) implements the
  policy-request decoder and client-side daily guardrail check that
  allotment-consuming callers must apply before creating an agent spend. It also
  decodes in-band `quicksilver.agent_payment_receipt` artifacts that refresh the
  known funding outputs after desktop handoff.
- **Receipt store:** [allotmentstore.h](../../src/agent/allotmentstore.h) persists
  `quicksilver.agent_payment_receipt` artifacts in a genesis-bound local store so
  the command-line agent can remember handoff funding and post-spend change
  outputs across invocations. The same store keeps an append-only output activity
  ledger for imported, spent, and change receipt events.
- **Desktop key handoff:** the vault validates a desktop-issued policy request
  against its recorded setup, proves the funding address is spendable by that
  vault, and exports a policy-plus-funding-key bundle for the gateway handoff.
  This is key material, not protocol enforcement. The same setup row can copy a
  `scantxoutset start ["addr(...)"] | quicksilver-agent ... importrecovery`
  command for recovering spendable outputs at the reserved funding address
  through a full node and importing them into the receipt store.
- **Command-line consumer:** [quicksilver-agent.cpp](../../src/quicksilver-agent.cpp)
  exposes `status`, `initheaders`, `requestheaders`, `processheaders`,
  `processinv`, `processtx`, `processaddr`, `processaddrv2`,
  `importnodeaddresses`, `announcetx`, `sendtx`, `addpeer`, `removepeer`,
  `listpeers`, `discoverpeers`,
  `syncheaderspeer`,
  `sendtxpeer`, `checkpolicy`, `checkbundle`, `signbundle`, `importreceipt`,
  `importrecovery`, `scanreceipts`, `listreceipts`, and `listreceiptactivity`.
  Message commands accept hex-encoded P2P payloads from `-message=<hex>` or
  standard input and print queued outbound payloads for an external transport.
  `checkpolicy` accepts a desktop-issued policy request plus a proposed spend
  amount and exits successfully only when the request decodes for the active
  chain and the spend fits the recorded daily guardrail.
  `checkbundle` accepts the desktop policy-plus-key bundle plus stored and
  optional `-paymentreceipt=<json>` artifacts, verifies that the WIF funding key
  maps to the bundle funding address, reports the merged funding-output count and
  total, and then applies the same policy check without printing the secret.
  `importreceipt`, `importrecovery`, `scanreceipts`, `listreceipts`, and
  `listreceiptactivity` manage and inspect the local receipt store, including
  optional receipt metadata such as `payment_id`, `label`, `memo`, and `payer`.
  `scanreceipts` imports new JSON receipt artifacts from a deterministic local
  inbox directory, making receipt discovery a repeatable filesystem handoff
  instead of a one-receipt paste step. The desktop receipt reviewer can write a
  valid pasted receipt into that inbox, and a funded setup row can write receipt
  artifacts for its current spendable funding outputs. The desktop invokes the
  same scanner directly, refreshes the visible durable UTXO count and total, and
  retains a copied `scanreceipts` command for external-host workflows. Spent
  activity suppresses stale inbox artifacts, so rescans cannot restore a
  consumed output.
  `signbundle` imports the same bundle and receipts into an in-memory signing
  context, checks the requested spend against the guardrail, selects known
  funding outputs when explicit UTXO arguments are not supplied, adds change back
  to the funded agent address, and signs it. Proving reads the anchor's congestion
  multiplier out of the header chain (see "The congestion multiplier" above), so
  `-prove=1` works at any anchor a thin client has synced, not only at genesis and not
  only under a full node. The full-node desktop consumer supplies the connected anchor
  and uses the same primitive. The desktop can paste a bundle, destination,
  spend amount, and optional already-spent amount, then save that bundle under
  the local agent datadir and copy a `signbundle` command that reads the saved
  file. On
  successful spends it removes consumed stored receipts,
  records spent-output activity, stores the change receipt when change is
  created, records change-output activity, and prints that `change_paymentreceipt`
  artifact for external handoff.
  Imported metadata is preserved in the spendable receipt list and copied into
  receipt activity rows, so operators can reconcile receipts after a spend rotates
  the live output set to change.
  `addpeer`, `removepeer`, and `listpeers` manage a genesis-bound local relay
  peer store under the active network datadir by default. `processaddr` and
  `processaddrv2` import decoded address-gossip payloads into that store for
  external transports. `importnodeaddresses` imports the JSON array printed by
  `quicksilver-cli getnodeaddresses`, and the desktop can call the same node
  address source through `interfaces::Node` to seed the local agent relay store
  without a command-line pipeline or manually typed peer. `discoverpeers`
  connects to explicit
  `-peer=<host[:port]>`, the stored configured peers, or the active network's
  fixed seeds, completes the same
  minimal version/verack handshake, asks cooperative peers for address gossip,
  and imports valid advertised peers. `syncheaderspeer` uses the same explicit,
  stored, or fixed-seed peers to run a live `getheaders`/`headers` exchange and
  save the accepted thin header store. `sendtxpeer` accepts the `tx_payload`
  produced by `signbundle` or the desktop spend reviewer, connects either to an
  explicit, stored, or fixed-seed peer, completes a minimal
  version/verack handshake with the active chain's message start bytes, and sends
  the transaction message over the socket. This closes the manual relay handoff,
  persistent configured-peer lifecycle, local-node addrman import, cooperative
  address-gossip import, configured-peer header refresh, and fixed-seed
  cold-start bootstrapping. Fixed onion seeds use the SOCKS5 endpoint configured
  through `-proxy` or `-onion`.
  `importrecovery` accepts the JSON result printed by `scantxoutset start`,
  validates recovered unspents for an explicit funding address, converts them to
  payment receipts with recovery metadata, and imports only new outputs into the
  local receipt store.
- **Regression coverage:**
  [agent_headerchain_tests.cpp](../../src/test/agent_headerchain_tests.cpp) covers the
  header chain/store/sync driver, message adapters, peer set, client facade, and
  CLI-style payload decoding paths.

The local v1 desktop flow is now end to end: durable receipt refresh feeds
in-process bundle signing, signed-spend review feeds a lifecycle-guarded
background relay, and configured-peer failure can fall through across the peer
set. Remaining integration gaps are deliberately outside that local flow:

- network-native payment discovery beyond the shipped receipt handoff and
  recovery imports (intentionally deferred for v1);
- authenticated orchestration of a separate or remote agent process. The v1
  boundary remains the saved-bundle and copied-command handoff because an
  authenticated API/daemon is intentionally deferred.

## Payment discovery

Without block scanning the client cannot notice arbitrary incoming payments. It
learns about them in band, from the payer, at the time of payment.

The blind spot is unsolicited payment from a stranger. Recovery is a one-off
`scantxoutset`, which reads the UTXO set rather than history and therefore works
against a pruned node. This recovers balance, not transaction history — see
"Backup is the only history" below.

## Agent allotment

An agent spends from an allotment derived from the user's vault as a child key. The
user retains the parent key, so funds can always be swept back without the agent's
cooperation. The user funds the allotment, sets a spending limit and a transaction
count, and can return unspent funds to the vault. Both directions cost a full
proof-of-work grind.

### Keys are shared, and this is stated plainly

The agent holds the allotment's keys. This is a deliberate choice, and it has a
consequence that the interface must state rather than obscure:

**Spending limits are client-side policy, not protocol guarantees.** The limits
are enforced by the agent's own client. An agent that ignores its client can spend
the entire balance. No interface text may describe them as guaranteed, enforced,
or protected.

Two distinct risks are disclosed at allotment creation, and acceptance is recorded:

1. **A dishonest agent** can spend the funds it was given. The user chose the
   agent and chose the amount; the exposure is capped at what they funded.
2. **A compromised agent host** hands the keys to a party the user never
   evaluated. This is the more serious of the two, because the user never made a
   judgement about that party at all.

There is no recovery mechanism for either. The design does not pretend otherwise.

## Why the client is not a daemon on the user's machine

An alternative was considered: run the client beside the vault on the user's own
machine and let the agent drive it remotely through an intent-level API. That
would give hard limits, real revocation, and no keys on agent infrastructure.

It was rejected because a remote allotment's blast radius is capped by physics — the
agent's host has no path to the vault at all. A listening, key-holding service on
the vault machine replaces that separation with a software boundary on the machine
that holds everything.

Splitting keys across processes does not repair this. It defends against theft of
the daemon's key; it does not defend against compromise of the machine by way of
the daemon. An attacker with code execution waits for the vault to unlock, reads
memory, logs keystrokes, or replaces the binary. Process isolation on a desktop
operating system is a speed bump, not a boundary.

A daemon remains reasonable when it is **not** next to the treasury — on a second
machine the user controls. That is deferred rather than refused: it is purely
additive against the same core, so adding it later creates no migration debt.

## Coordination between vault and agent

Shared keys mean the vault and the agent can select the same outputs. There is no
daemon and therefore no coordination channel, so the relay pool serves as one: the
vault computes available balance as confirmed outputs minus those already spent by
transactions in the relay pool.

The residual race is a transaction whose grind has started but which has not yet
been broadcast — a window of roughly the grind duration. A collision there wastes
work. It does not produce an incorrect balance or a double spend.

**Decided 2026-08-04: the vault locks the agent's funding outputs at the moment it
exports them.** `CVault::ExportAgentAllotmentPolicyBundle` hands over the funding key
*and* the list of outputs that key can spend; from that instant both sides can sign
for them. So the same call now locks each of those outputs, persisted to the vault
database, which removes them from ordinary coin selection — `AvailableCoins` skips
locked outputs unless a caller opts out, and only the coin-control listing does.

This makes the collision structurally impossible for exported outputs rather than
merely improbable, and it needs no protocol, no daemon and no shared state, which
matters because the vault and the agent are on different machines by design. The
alternative considered — giving the agent its own derivation path and teaching the
vault's coin selection to avoid it — was rejected because it does not match what
ships: funding is an explicit set of outputs handed over in a bundle, not a
sub-account, and the lock reuses a mechanism that already exists and is already
persisted.

Two consequences, both intended:

- **The vault's spendable balance drops when a bundle is exported, and the same amount
  appears as a delegated balance.** Locking an output removes it from coin selection but
  not from ownership, so reporting one number without the other would be wrong in both
  directions: counting delegated coins as available produces a figure a transfer then
  refuses to honour, and dropping them silently reads as money that vanished. `GetBalance`
  therefore returns `m_mine_delegated` alongside `m_mine_trusted`, the overview shows a
  *Delegated* row when it is non-zero, and `getbalances` reports `mine.delegated`. The
  total the vault claims to own is unchanged. Coin control still displays the outputs,
  marked locked, so an explicit sweep remains available — which is what "the user retains
  the parent key" is for.
- **Re-exporting a bundle still enumerates the agent's outputs**, because the export
  path deliberately does not skip locked outputs when it builds the list. Otherwise a
  refreshed bundle would tell the agent it holds nothing.

The delegated figure is derived, not stored: it is the value of the unspent outputs that
are both locked and paid to an agent's reserved funding address. Nothing locked means
nothing delegated, which is what lets a vault that has never funded an agent skip the
lookup entirely.

The residual race above is unchanged for outputs the vault sends to the funding
address *after* an export and before the next one. Those are not yet locked, and the
relay pool remains the only coordination for them.

## Backup is the only history

A thin client cannot rescan. The vault file is therefore the only complete record of
the user's spending keys and transaction history. The application does not issue a
recovery phrase, and `scantxoutset` cannot recreate lost keys or past activity.
Interfaces built on this core must treat backup as a standing concern rather than a
settings item.

## Closed problems

Both of this document's former open problems now have answers.

- ~~**Peer discovery.**~~ **Resolved.** `vFixedSeeds` is populated on `main` and
  `publictest` from [contrib/seeds](../../contrib/seeds), carrying an onion seed run on
  the maintainer's own hardware, and [bootstrapping.md](../bootstrapping.md)
  documents the `-addnode` path for anyone who would rather not use it. A cold
  client can find the network.
- ~~**Grind delegation.**~~ **Decided, scoped out of v1.** Grinding is trustlessly
  delegatable — the pre-image excludes `scriptSig` and the witness and the grind
  runs before signing — but no in-protocol reward exists, so a grind market needs a
  payment mechanism designed from scratch. v1 treats a GPU as the practical
  transfer path. The command-line agent uses one on live networks unless the
  operator passes `-allowcputxpow`, which is off by default because an agent
  spend can start while the computer is in use. See
  [delegation.md](delegation.md).

What v1 deliberately does not build, with trigger conditions, is in
[v1-scope.md](v1-scope.md) — including the gateway components this document's model
does not provide, and the daemon deferral described above.
