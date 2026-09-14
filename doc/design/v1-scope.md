# What v1 Does and Does Not Do

> Status: **decided** (2026-08-04). This document exists so that scope questions stop
> recurring. Everything listed as out of scope has a reason and a trigger condition — the
> thing that would make the project revisit it. A deferral without a trigger is just
> silence, and silence gets rediscovered as a surprise.

## 1. Adoption: how agents come to use this

Quicksilver's intended users are software agents, which raises an obvious question that
has been open since 2026-07-20: how does an agent come to use it at all?

Most of that question is distribution and go-to-market, and it is outside this program.
The part that is genuinely technical is narrow: **does anything have to exist in v1 for
an agent to discover Quicksilver?**

**Decision: no protocol surface for discovery ships in v1.** No well-known endpoint, no
manifest, no registry. What ships instead is documentation — a bootstrapping guide
([bootstrapping.md](../bootstrapping.md)) and the agent client's own command surface,
which is enough for a human integrator to connect an agent deliberately.

The reasoning is the test any adoption answer has to pass: **recipients versus willing
users.** A discovery mechanism built before anyone wants to be discovered produces
nothing to discover. The Nano lesson is the same one from the other direction — a coin
distributed free to people who did not ask for it produces recipients, not a network.
Both failure modes come from manufacturing the appearance of demand.

A well-known endpoint is also not free: it is a permanent protocol surface, published at
launch, that cannot be removed later without breaking whoever adopted it.

**Trigger for revisiting:** an integrator who wants to connect an agent and cannot,
because the missing piece is mechanical discovery rather than documentation. One such
report is worth more than any amount of anticipating.

## 2. The Agent Allotment Gateway

The 2026-07-29 repositioning described a gateway with six components. Measured against
`src/agent/` as it stands, rather than against the design document:

| component | status | what actually exists |
| --- | --- | --- |
| policy engine | **partial** | `CheckAllotmentSpend` enforces a funding limit and a daily limit (`allotmentpolicy.cpp`), applied by the `checkpolicy` and `checkbundle` commands |
| capability issuance | **partial** | the vault exports a policy-plus-funding-key bundle (`CVault::ExportAgentAllotmentPolicyBundle`). This is a key handover, not a capability token |
| revocation | **absent** | no revocation path exists. The only way to withdraw an agent's authority is for the user to sweep the funds back with the parent key |
| durable job queue | **absent** | nothing |
| receipts | **shipped** | `allotmentstore` persists payment receipts and their activities, and `checkbundle` merges them |
| authenticated API distinct from RPC | **absent** | `quicksilver-agent` is a command-line tool. It does not listen |

So the gateway is roughly one third built, and the third that is built is the part that
needed no new trust model.

### What may and may not be claimed

**True, and free to say:** the GPU worker never receives private keys. Quicksilver grinds
before signing and verifies the returned proof, so the work can be done by a party that
cannot alter or steal the payment. This is backed by existing code — see
[delegation.md](delegation.md) §1.

**Must never be said: that limits are guarantees.** Keys are shared with the agent by the
user's own explicit decision, so a limit is client-side policy that the agent's own
software applies to itself. An agent that ignores its limits is not stopped by anything.
The interface says this in as many words — the word *guarantee* appears once on the agent
screen, negated (`src/qt/agentallotmentpage.cpp`) — and it must stay that way.

The honest summary of what a user gets: a funded allotment whose spending the user can
review, with receipts, and whose blast radius is bounded by how much was funded into it —
not by a limit the protocol enforces.

**Trigger for revisiting:** revocation is the component whose absence matters most,
because it is the difference between "I have decided to stop this agent" and "I must move
the money before it spends again." It becomes worth building when agents run
unattended for long enough that sweeping is not a timely response.

## 3. Block-capacity scaling

Deferred, and the deferral's reasoning needs correcting against what the
byte-pricing work actually shipped.

**The mechanism.** Required transaction work is `base × (bytes·U + Δutxo·R_b) / (R_b·U)`,
where `base = BaseTxWork(anchor)` is the mean block work over the retarget window divided
by `K` = 106, multiplied by a congestion factor. Mean block work rises with total network
hashrate, because difficulty retargets to hold 5-minute blocks. **So a user on fixed
hardware sees their grind time grow in proportion to the network's hashrate**, without
limit. That is the problem, and it is real.

**The correction.** The earlier framing said the only escape lever is block capacity. That
is at most half right after byte pricing. Required work has two parts, and capacity only
touches one of them:

- The **base coupling** — `mean_block_work / K` — does not depend on block capacity at
  all. Raising `MAX_BLOCK_WEIGHT` does nothing to it.
- The **congestion multiplier** rises when blocks are full. Bigger blocks do relieve
  this, and only this.

So bigger blocks relieve congestion pressure and leave the underlying growth untouched.
The levers that actually reach the base term are `K` — fixed by mint safety, holding a
4.13x margin, and not available — and the reference constants `R_b` and `U`, which set
how many bytes cost one unit of base work. Raising `R_b` is a decision to make
transactions cheaper in work and therefore to make filling the chain cheaper by the same
factor, which is precisely the coupling [chain-growth.md](chain-growth.md) exists to
document.

**Nothing changes in v1.** At launch there is no network large enough for this to bind:
the difficulty floor is what binds until roughly 28 GPUs, and below that a transaction
costs one cycle regardless.

**Trigger for revisiting:** a measured median transaction grind exceeding **10 minutes on
one commodity GPU** — about twelve times the 0.83 minutes measured at the floor. That is
a number that can be watched rather than argued about. When it is crossed, the analysis
must start from the base term, not from block size.

## 4. The agent-client daemon

Deferred, with reasoning that holds and has never been written down publicly.

A remote allotment's blast radius is capped by physics: the agent's host has no path to the
vault, so compromising it reaches only what was funded into the agent. A listening,
key-holding service running on the vault machine replaces that air gap with a software
boundary on the box that holds everything. Splitting keys across processes does not repair
this — it defends against theft of the daemon's key, not against compromise of the machine
by way of the daemon.

Adding a daemon later is purely additive against the same core, so it creates no migration
debt and does not conflict with the project's rule against shipping deprecated paths.

**Trigger for revisiting:** users who have a second machine they control. A daemon is
reasonable when it is not next to the treasury, so the trigger is the deployment shape,
not the feature request.

## 5. Also decided elsewhere

| decision | where |
| --- | --- |
| a GPU is the practical transfer path; no grind market | [delegation.md](delegation.md) |
| consensus nodes prune by default; maintainer runs one archival node | [chain-storage.md](chain-storage.md) |
| encrypted history export ships; automatic backup does not | [vault-backup.md](vault-backup.md) |
| the vault locks an agent's funding outputs at export | [agent-client.md](agent-client.md) |
