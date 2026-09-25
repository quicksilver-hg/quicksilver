# Desktop Application

> Status: navigation approved. Qt launch page, consensus review/cost disclosure,
> persisted consensus acceptance, mining gating, and standalone mining setup are
> implemented. The Qt shell now defers consensus `AppInitMain()` until opt-in.
> The opt-in handoff now shows an explicit consensus-starting state until the
> node client model arrives. A lazy startup failure now clears the saved opt-in,
> runs partial core cleanup, keeps the vault-first shell open, and marks
> consensus as restart-required instead of quitting immediately. The launch card
> now refreshes the active vault balance and recent activity summary for
> returning users. The Network page now exposes the developer network context,
> including the active token, datadir locations, restart consequence, and the
> persistent non-live-network banner. Developer network cards now include
> restart controls that relaunch the application with the requested `-chain`
> token, with focused Qt coverage for replacing prior network selectors while
> preserving unrelated startup options and for the confirmation dialog handoff.
> A standing backup prompt now appears on the launch page and active vault
> overview once a vault is active, routes through the existing vault backup
> dialog, and records completion in the vault database so it follows the vault
> file instead of the desktop's Qt settings. The transfer
> screen now discloses that sending spends proof-of-work time,
> and repeats that disclosure in the final confirmation. The send flow now shows
> an active proof-of-work progress panel with elapsed time while the desktop
> prepares/proves the transfer, then reports the completed graph-attempt count
> once the proof is ready for review. The send screen now also distinguishes
> fast sandbox proof-of-work from the live/public CPU fallback, recommends a
> configured GPU solver for practical transfer times, checks that a configured
> solver path resolves to an executable file, and asynchronously probes executable
> solver startup before describing it as ready. The Qt English translation source
> catalogs have been regenerated for the launch,
> consensus, network, mining, backup, send, and agent desktop strings. The Qt
> vault now has an Agents section with shared-key risk disclosure, limit inputs,
> vault-backed queued setup records, a funding handoff that prefills the
> transfer screen with the setup address and remaining requested amount, and
> derived funding confirmation from spendable outputs at the reserved address
> that disables funding once the requested amount is present. The first
> visual-detail pass now gives the launch, consensus, network, mining, backup,
> send, and agent surfaces a shared dark desktop treatment with framed panels,
> status chips, and distinct
> primary/secondary actions. The no-vault desktop surface now offers explicit
> create and open-existing vault actions, with the open action routed through the
> existing vault controller picker. The vault loader is now constructed during
> base interface initialization, and the Qt vault controller can attach before
> consensus starts; vault models receive the consensus client model later if the
> user opts in. The launch vault summary now follows the persisted mask-values
> setting and offers the same hide/show balance choice directly on the
> returning-user card. Focused Qt coverage now proves the controller is created
> by base initialization before consensus startup and is the same controller
> attached to the shown shell after consensus initializes. Balance polling now
> keys its refresh guard off the vault interface's tracked block hash instead of
> the consensus client model, with Qt coverage for the no-client-model path.
> The nonblocking vault-interface tip probes now fail closed while a vault has
> no initialized chain/header tip, including transaction detail lookups, with Qt
> coverage for the pre-consensus path. Transfer preparation now asks the vault
> interface whether transaction creation has live chain/header state before
> touching balance selection or spend construction, and reports a temporary sync
> wait instead of entering chain-bound send code too early. Coin-control output
> enumeration and agent funding checks now use the same pre-consensus
> availability boundary, so those Qt paths stay empty/zeroed instead of forcing
> chain-backed coin scans while the vault tip is unavailable.
> The vault-first desktop now watches the validated `agent/headers.dat` store
> (or `-headerstore`) and applies its height, hash, and time through the vault
> loader whenever no live consensus chain is attached. The verified genesis is
> the initial tip when the store does not exist yet. Vault balance polling stays
> active without a consensus client model, so imported/local transactions and
> confirmation depths refresh against that thin tip. Header state deliberately
> does not enable transaction creation by itself: node-free spend-policy inputs
> and desktop-vault UTXO discovery remain part of the unfinished node-free core.
> Agent setup acceptance and
> requested guardrails are now recorded in the vault database with a reserved
> vault funding address and explicit pending policy status. The desktop can
> confirm when that address is funded and copy a JSON Agent Allotment Gateway bundle
> containing the policy request, reserved funding key, and current spendable
> outputs for funded setups.
> The command-line agent can now decode that bundle, verify that the funding key
> matches the funding address, check a proposed spend against the daily
> guardrail, and create a signed/proved transaction from bundle funding outputs
> or persisted/in-band payment receipts unless explicit UTXO metadata is
> supplied, and it rotates stored receipts by replacing spent outputs with
> post-spend change receipts. The desktop can now paste and locally review a
> returned signed spend from raw transaction hex or `quicksilver-agent` output,
> then submit it through the active consensus node. The command-line agent now
> keeps output-level receipt activity for imports, spent outputs, and change
> outputs. Rich payment request metadata is preserved by the command-line
> receipt store, the command-line agent can scan a local receipt inbox, the
> desktop can locally review payment receipt artifacts, save valid receipts into
> that local inbox, save current setup funding receipts into the same inbox, scan
> it in-process, and show the durable spendable-output count and total. The
> shared scanner treats spent activity as authoritative, so retained inbox files
> cannot resurrect consumed UTXOs. External-host workflows can still copy the
> matching `scanreceipts` command. Reviewed signed spends can save the
> agent's returned change receipt into that inbox, copy relay-ready payloads, or
> copy a peer-specific `sendtxpeer` command for node-free transport handoff. The
> desktop can also save a pasted agent bundle locally and copy a runnable
> `signbundle` command for the requested destination and amount.
> The command-line agent can now store
> configured relay peers and relay a reviewed signed transaction to either an
> explicit peer or the stored peer set, and the desktop can directly import
> local-node addrman entries into that agent relay store. The command-line agent
> can also import `getnodeaddresses` output, import address-gossip payloads, or
> ask configured cooperative peers for more peers. It can refresh the thin header
> store from configured peers, and the desktop can copy the matching
> `syncheaderspeer` command. The desktop can also invoke the same bundle-output
> signing path in-process, merge persisted agent receipts, rotate spent/change
> receipt-store entries, load the resulting relay payloads into signed-spend
> review, and relay the reviewed spend on a background worker to the typed relay
> peer, stored peers, or active network fixed seeds. The GUI stays responsive
> while every selected peer is attempted and reports partial failures when at
> least one peer accepts the transaction. The agent's live network commands now
> also fall back to the active
> network's fixed onion seed when neither an explicit nor stored peer exists;
> `-proxy` or `-onion` provides the required SOCKS5 path.
> Recorded agent setups can also copy a `scantxoutset` recovery
> pipeline that imports recovered outputs into the agent receipt store.
> Persisted-consensus startup now leaves startup vault ownership to
> `AppInitMain()` instead of opening the settings-listed SQLite vault in the GUI
> first. The assembled shell is globally scrollable when a work area is smaller
> than its content, and its regression is measured on the real main window
> against a 1080p work area rather than on one isolated page.
> The desktop packaging boundary now keeps the Qt application as the
> user-facing artifact while routing developer executables
> separately. This document describes the intended application except where noted.

Quicksilver ships one desktop application for every kind of user, not a set of
binaries per role. It is the permanent control surface for the humans who are
accountable for what their agents do, and it is the first thing a person who has
never used a cryptocurrency will open.

## The problem it solves

The inherited project ships a graphical wallet, a command-line tool, a daemon, and
several networks, and expects the user to work out which of them they need. For a
developer that is a reasonable trade. For someone who wants to hold quicksilver it
is a wall, and it is a wall people bounce off.

This application makes the developer surface optional and easy to reach, and keeps
it entirely off the path of someone who only wants to open a vault.

## Capabilities, not modes

The application exposes three capabilities. They compose, so "I want two of these
at once" is not a navigation problem.

| Capability | Availability | Requires |
| --- | --- | --- |
| Vault | Always present | — |
| Consensus | Opt-in | — |
| Mining | Opt-in | Consensus |

Presented in that order, so the dependency reads top to bottom. Mining is shown
locked rather than hidden — hiding it makes people search for it, whereas showing
it locked teaches the rule at a glance. The locked navigation item remains
clickable and opens the consensus cost and consent explanation; a disabled item
with no response would teach nothing.

Each capability carries a state word rather than a switch, because two of the
three are real commitments rather than preferences.

### The vault does not embed a node

The vault runs on the agent client's thin core — see
[agent-client.md](agent-client.md). It follows headers, stores no chain history,
and is usable within seconds of first launch.

This is the load-bearing decision in the whole application. If the vault embedded
a node, the ordinary user would inherit the same storage cost that the agent
client exists to avoid — about 26 GB per year pruned, 154 GB per year archival
(see [chain-storage.md](chain-storage.md)) — and first launch would begin with a
download measured in days.

The node stops being the application's foundation and becomes a component
provisioned on demand.

## Launch

The launch screen shows the brand and the three capabilities. It carries no
network name; see [network-naming.md](network-naming.md) for why the live network
is unnamed on the default path.

It persists after a vault exists. The vault entry then also carries a summary —
balance, agent setup funding state, and time of last activity — which answers the question a
returning user actually has before they can ask it: *my vault was here yesterday,
is it still there and did anything happen to it?* Balance alone establishes that
the vault exists; recent activity establishes that nothing happened while they
were away.

Opening directly into the vault would answer the same anxiety by permanently
hiding the other two capabilities, which is a worse trade.

**Three copy decisions taken 2026-08-04, recorded here so they stop being
reopened:**

- **The balance stays visible by default.** A hide/show control exists and obeys
  the persisted mask-values setting, so a user who wants the balance hidden can
  have it; the default is not that. The argument for hiding it was that the card
  answers a question about *existence*, which last-activity already answers
  without exposing an amount to anyone glancing at the screen. That argument is
  sound and was not chosen: a returning user reading their own machine should not
  have to click to see whether their money is there.
- **The subtitle stays as written** — *"Vault first. Consensus only when you
  choose it."* The slot was once deliberately empty; it is now filled and the copy
  states the application's actual priority, which is what the slot was for.
- **Branding remains as shipped.** Alternative branding ideas were raised and the
  current copy was kept.

Current Qt status: the launch vault card now says "Create or open vault" until a
vault is active, then says "Open vault" for returning users. When no vault is
active, the no-vault state provides separate create and open-existing controls;
the vault loader is constructed during base interface initialization, so the Qt
vault controller can attach before consensus `AppInitMain()` starts. Opening
still uses the existing vault controller and vault database path; the consensus
`ClientModel` is attached later when the user opts in. When a vault is active,
the launch balance obeys the persisted mask-values setting and the
returning-user card offers a hide/show balance control wired to that same
setting. Focused application coverage now asserts this controller exists after
`baseInitialize()` and before `requestInitialize()`, then remains attached to the
shown window after consensus initialization completes. Vault balance polling now
uses a nonblocking vault-interface block-hash marker before recomputing balances,
so the Qt model can refresh from vault-tracked header progress before a
consensus `ClientModel` exists; focused Qt coverage proves the no-client-model
polling path skips unchanged markers and refreshes when the vault marker changes.
The same interface boundary now treats an uninitialized pre-consensus vault tip
as temporarily unavailable rather than asserting, and focused Qt coverage proves
the balance, transaction-status, and transaction-detail probes fail closed before
consensus/header progress has initialized the vault tip. The transaction details
view now shows a temporary header-tip waiting state instead of calling the
blocking details path while the vault-first shell is open without consensus.
Transfer preparation uses the same boundary before balance selection or spend
construction, so a pre-consensus vault returns a temporary header-sync wait and
does not enter chain-bound transaction creation until the vault reports ready.
Coin-control output enumeration now also uses nonblocking vault output probes, so
opening coin control or recalculating selected-output labels before consensus
startup does not enter the blocking coin-list path.

## Being behind is known work, not an old clock

The desktop decides it is catching up when a header chain better than its tip is
known and not yet connected. It does not decide from the age of the tip.

The inherited rule — a tip older than ninety minutes means you are behind — is
sound for the chain this one forked from, running since 2009 and producing a
block every ten minutes without interruption. It is wrong for a chain that
starts at height 0.
The genesis timestamp is fixed when the parameters are locked, so the first
operator's tip is "old" from the moment they download the software and never
stops being so. A full launch-readiness walkthrough caught exactly that: the
only node on the chain, with nothing anywhere to sync from, was shown a modal warning that its
balance might be incorrect, over a table reading 100% complete and "blocks left:
unknown".

This is the same rule the node itself already applies before it will mine
(`node::MiningShouldWaitForSync`), which compares chain work rather than clocks.
The desktop's reading of it is `ModalOverlay::isBehindKnownHeaders`, comparing
the best known header height against the connected tip; the overlay already
tracked both. `MAX_BLOCK_TIME_GAP` had no other caller and is gone.

The first screen's prose follows from the same fact. It no longer promises to
"download and process the full block chain": on launch day there is nothing to
download, and the figure beside that sentence is a projection of what the first
year is expected to add, not the weight of a pending transfer. The wording has
to stay true a year later, when there is history to fetch, so it names both.

## A granted shutdown is not parked behind a dialog

`SIGTERM` and the `stop` RPC both raise the node's shutdown token. Only the
daemon waits on that token directly; the desktop's main loop is Qt's, so a
200 ms poll turns the request into a Qt quit. That poll used to skip every tick
while a modal dialog was open, and nothing anywhere reported the skip — `stop`
answered *"Quicksilver stopping"* and returned success, `SIGTERM` was caught and
dropped, and the application ran on.

The same walkthrough found a reference desktop in exactly that state: twelve
minutes of repeated `SIGTERM`s and one `stop`, with a leftover *"Transfer proof-of-work canceled"*
message box on screen and not one shutdown line in `debug.log`. It exited within
five seconds of that box being clicked.

The poll now closes the modal instead of skipping the tick, and takes the
shutdown on a later tick once the dialog's nested event loop has unwound —
`requestShutdown()` deletes the models and controllers that loop may still be
standing on, so tearing down underneath it is not an option. The guard's original
purpose is kept: with no shutdown requested, a dialog is the user's and is left
alone.

This matters beyond tidiness. A desktop session ending sends `SIGTERM` and then
`SIGKILL`, so the old behaviour made every logout an unclean kill of a node
holding LevelDB and SQLite open. It also gave any agent or operator using the
documented stop path a success code for something that did not happen.

## Costs are disclosed above the button

Two screens impose a real cost on the user, and both state it before the control
that accepts it, never after.

- **Sending** performs a proof-of-work grind taking minutes. This is what replaces
  a fee, so the screen shows work being done and counts it in graphs solved,
  making the delay legible as progress rather than as a hang. Nothing is deducted:
  the full amount arrives. The screen states that live and public-test transfers
  are practical with the external GPU solver and may take many minutes on the
  desktop's CPU fallback. The command-line agent uses that solver on those
  networks unless `-allowcputxpow` is set. The flag is off by default because an
  agent can start the work while the computer is in use. See [delegation.md](delegation.md).
- **Consensus** costs about 26 GB per year pruned, which is the default, or 154 GB
  per year to keep full history. Both figures are shown, because a user who is
  offered only the larger one will decline for the wrong reason. It is framed as
  buying independent verification rather than unlocking features, because nobody
  should enable it believing it grants something they need. Declining is offered
  with equal weight, since for most users declining is correct. See
  [chain-storage.md](chain-storage.md).

Burying either would be the same dishonesty as concealing a fee.

Current Qt status: the transfer form now states the proof-of-work cost above the
send controls, distinguishes sandbox proof-of-work from live/public CPU fallback
and `-cuckatoosolver=<path>` acceleration, shows an active proof-of-work panel with
elapsed time while the async prepare/prove worker runs, forwards GPU solver
heartbeats into live graph-attempt updates when the external solver reports
them, reports the completed graph-attempt count from the solved proof nonce, and
repeats the disclosure in the confirmation dialog. The GPU-solver disclosure now
distinguishes an absent startup argument, a configured path that is missing, a
configured path that is not executable, an executable file awaiting probe, an
active startup probe, a passed startup probe, a failed startup probe, and a probe
timeout. CPU fallback now emits the same latest-nonce progress callbacks from its
in-process attempt loop, so sandbox CPU proof-of-work also moves the
graph-attempt text before completion. Before any proof work starts, transfer
preparation now fails closed when the vault cannot create transactions from live
chain/header state yet, with focused Qt coverage proving that no balance
selection or transaction-creation call is made on the pre-consensus path. The
coin-control selected-output summaries use the same guarded output-detail probe
before send preparation.

## Agent allotments

Agents are a section of the vault rather than a separate mode, because they are
funded from it.

Creating one presents the shared-key risk in full — both the dishonest agent and
the compromised host, in that order of increasing severity — and records the
user's acceptance. The word *guarantee* appears once, negated. Limits are captured
as inputs but described as a guardrail, so nothing on screen implies enforcement
the protocol does not provide. The confirming control restates the risk rather
than reading "Create". See [agent-client.md](agent-client.md) for the model.

Current Qt status: the vault contains an Agents section with the shared-key risk
copy, funding and daily guardrail inputs, and the acceptance checkbox. Once the
form is complete, the confirming control records the user's acceptance,
requested guardrails, and a reserved vault funding address in the vault database
and lists the queued setup in the vault UI. Each queued setup can now hand off to
the normal transfer screen with its reserved address and remaining requested
funding amount prefilled, and the agent page plus launch summary now derive
funding confirmation from spendable outputs at that reserved address. A partially
funded setup offers only the shortfall; a fully funded setup shows completion and
does not offer another funding transfer. Agent setup records now carry an
explicit policy status and the setup list shows "Policy pending" separately from
funding state, so a funded setup is not mistaken for an enforced agent allotment.
Funding confirmation now fails closed to zero when pre-consensus header state is
not available for output enumeration. Once the reserved address has the
requested funding, the row can copy a JSON Agent Allotment Gateway bundle from the
vault/model boundary containing the setup id, funding address, requested
guardrails, current funding evidence, chain token, genesis hash, request creation
time, current spendable funding outputs, and the reserved funding private key for
the agent gateway handoff. The desktop page can paste the inner policy-request
artifact back through the vault/model boundary to review validated metadata or
show validation failures, including malformed, cross-chain, unknown-setup, and
stale-metadata requests,
with distinct ready, valid, and error feedback states. The command-line agent can
now import the bundle into an in-memory signing context, enforce the guardrail
for a requested spend, select the bundle's spendable outputs when explicit UTXO
arguments are not supplied, merge in-band payment receipts for post-handoff
funding outputs, build change back to the agent funding address, prove the
transaction, sign it, and print raw transaction hex plus relay-ready `tx` and
`inv` payloads. The command-line agent can also import/list persisted payment
receipts, consume those stored outputs on later spends, and replace spent stored
receipts with post-spend change receipts. Stored and in-band receipt artifacts
can carry optional `payment_id`, `label`, `memo`, and `payer` metadata, and the
agent preserves that metadata in the receipt list and output-activity record.
The command-line agent can also scan a local JSON receipt inbox and import only
new receipts, so operators can repeat the discovery handoff without rebuilding a
command line for each artifact. The desktop page uses the same genesis-bound
scanner on page entry, explicit refresh, local receipt writes, and a visible-page
poll. It displays the durable spendable-output count and total, and the shared
import path refuses to re-add an output already recorded as spent. The desktop
page can paste a payment receipt
artifact, validate it against the active chain metadata, display its funding
output plus optional metadata, save and import it through the same local scan
inbox, and copy the matching `quicksilver-agent ... scanreceipts` command for an
external agent host. A funded setup row can
also write payment receipts for its current spendable funding outputs into that
inbox, giving the agent a direct receipt-store import path for desktop-funded
setups without requiring the full key bundle for receipt discovery. The desktop
page can also paste an agent bundle, destination, spend amount, and optional
already-spent amount to sign the requested spend locally through the same
bundle-output signing core. The local path supplies the full node's connected
anchor, including its congestion state, so the proof uses the same target as
consensus. It merges persisted payment receipts and rotates the
local receipt store with spent-output and change-output activity. The generated
result is loaded into signed-spend review, and any generated change receipt is
also saved into the local agent inbox. It can still save that bundle under the
local agent datadir and copy a runnable
`quicksilver-agent ... signbundle` command for an external agent host. The
thin header store carries the congestion multiplier, so `-prove=1` works at
any header the agent has synced; `-prove=0` is offline-test-only. It can then
paste the agent's
command output or raw transaction hex, decode the returned signed spend, show
its transaction id, input count, output count, and output total, copy relay-ready
`tx`/`inv` payloads for an external node-free transport, or copy a runnable
`quicksilver-agent -peer=<host[:port]> sendtxpeer` command for manual
configured-peer relay. The desktop can also run that relay on a background
worker directly from the reviewed signed-spend panel, using the typed relay peer
when present, stored peers when the peer field is empty, or the active network's
fixed seeds when the store is empty. Relay controls remain guarded while the
worker runs, and completion reports full success, partial peer failure, or total
failure without blocking the GUI. If the agent output includes a
`change_paymentreceipt`, the review saves that receipt into the local agent
inbox so the next receipt scan can pick up the rotated change output. The
command-line agent also has `addpeer`, `removepeer`, and `listpeers` commands
for a persistent configured relay peer set, so
`sendtxpeer` can relay through stored peers when no explicit `-peer` is supplied.
The same desktop relay peer field can copy `addpeer` and `discoverpeers` command
handoffs, and `discoverpeers` can also run against the stored peer set when no
explicit peer is supplied. It can directly import the active desktop node's
addrman entries into the local agent relay store, and can still copy a
`quicksilver-cli getnodeaddresses | quicksilver-agent ... importnodeaddresses`
pipeline for an external agent host. It can also copy `syncheaderspeer` handoffs
for the explicit relay peer or the stored peer set, so the command-line agent can
refresh its thin header store without pasted `headers` payloads when configured
peers are available. The vault-first desktop consumes that same validated store
on a changed-file poll and lets a live consensus chain take authority when one
is attached.
It can still submit the reviewed transaction through the active consensus node
when that path is available. Each recorded setup can also copy a
`quicksilver-cli scantxoutset start ["addr(...)"] | quicksilver-agent ...
importrecovery` command for recovering spendable outputs at the reserved funding
address through a full node and importing them into the agent receipt store. The
command-line agent can list output-level receipt activity for imported, spent,
recovered, and change outputs, so a rotated-away receipt no longer disappears
from the local record. Its live discovery, header-sync, and transaction-relay
commands fall back to the active network's fixed onion seed when the local peer
store is empty; a configured `-proxy` or `-onion` SOCKS5 endpoint supplies the
Tor path. The remaining discovery gap is network-native payment discovery.
Authenticated orchestration of a separate or remote agent remains outside v1;
that workflow continues to use saved artifacts and copied commands.

## Visual treatment

The desktop shell should feel like one application, not inherited Qt screens with
new labels. Shared styling belongs at the shell level: page titles, card borders,
status chips, warnings, primary actions, and data panels should read consistently
across launch, consensus review, network context, mining, backup, send, and agent
allotments.

Current Qt status: the first visual-detail pass is implemented in the global
Quicksilver stylesheet and the new desktop pages. Launch cards, backup prompts,
consensus cost rows, network context cards, mining payout setup, send work
progress, and agent risk/setup panels now share the same framed treatment. The
pass also adds distinct primary and secondary button classes and focused Qt
assertions for the new desktop affordances.
The main content is also hosted in an as-needed scroll area, so title-bar and
taskbar space on a 1080p desktop cannot put the bottom of the application beyond
reach. The sizing check constructs `QuicksilverGUI` with its command rail,
status bar, and complete `VaultFrame` instead of testing a bare child page.

## Backup

Backup is a standing element of the vault, not a settings item.

A thin vault cannot rescan, so the vault file is the only complete record of its
spending keys and transaction history. The application does not issue a recovery
phrase, and `scantxoutset` cannot recreate lost keys or past activity. The prompt
remains until backup is done.

**Decided 2026-08-04: an explicit encrypted history export ships in v1**, rather
than the banner being a warning with nothing behind it — see
[vault-backup.md](vault-backup.md) for the mechanism, what it deliberately omits
(private keys), and why automatic backup was refused. The banner copy states the
actual boundary: only a current vault-file backup restores both spending keys and
history. The "already backed up" copy now says that a
backup holds only the history that existed when it was made, because a stale
backup that reads as done is the failure mode this design is most exposed to.

Current Qt status: the launch page and active vault overview show this prompt
for the active vault. A successful backup stores the completion marker in the
vault database before the file copy, so the marker follows the vault file rather
than this desktop's Qt settings.

## Developer networks

The developer entrance is a network context switch, not a fourth capability.
Network selection binds the datadir, ports, and chain parameters at startup, so
switching restarts the application.

The selector lists all three networks with their formal names, tokens, and datadir
paths, and a plain sentence about what each is *for*. It states the restart as a
consequence up front, and states that the live vault is unaffected.

Restart controls are explicit process restarts. They preserve the user's original
startup options where possible, replace any previous network selector with the
requested `-chain` token, and then use the normal shutdown path before the fresh
process opens.

A network that is not the live one carries a persistent banner, not a dismissible
dialog. Dialogs get dismissed and then forgotten by the time the confusion
arrives. Sandbox-only controls exist only on the sandbox, so there are no disabled
controls to explain elsewhere.

The Network page also makes Tor provisioning explicit. Installing a distribution
package is not treated as readiness: it names the normally disabled control port,
cookie authentication, the distinct 9050 SOCKS and 9051 control roles, and the
`-onion` or separate-proxy setting required for outbound onion peers. Detailed
permission checks remain in [tor.md](../tor.md).

## What ships

The user-facing artifact is one application. The repository currently builds ten
executables; the remainder are developer tooling and belong in a separate download
or a clearly separated subdirectory of the archive, not in front of someone who
came to open a vault.

Current packaging status: the Debian `quicksilver` package is the desktop
artifact and no longer recommends the daemon/tools package. The Windows installer
keeps `quicksilver` at the install root and places developer executables under
`developer-tools`; test binaries are not part of that installer.

## Open questions

- None in the current desktop pass. Node-free transaction construction,
  desktop-vault UTXO/payment discovery, and enforced agent allotment policy remain
  known implementation gaps above, rather than unanswered UI questions.
