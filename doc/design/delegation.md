# Delegated Proof-of-Work Grinding

> Status: **decided, scoped out of v1** (2026-08-04). Grinding is trustlessly
> delegatable today at the protocol level. There is no market for it, and v1 does not
> build one. A machine without a usable GPU can use the desktop's CPU fallback,
> but cannot transact at a practical speed; the application says so before the
> user commits to anything.

## 1. What is already delegatable, and why

The transaction proof-of-work pre-image commits to version, prevouts, sequences,
outputs, locktime, anchor height, anchor hash and the nonce. It does **not** commit to
`scriptSig` and does **not** commit to the witness — see `PowPreimage` in
[transaction.h](../../src/primitives/transaction.h). The vault grinds *before* signing
([vault/spend.cpp](../../src/vault/spend.cpp)).

Two properties follow, and both are structural rather than promised:

- **A grinder cannot alter the payment.** The outputs are inside the pre-image, so any
  change to who gets paid, or how much, invalidates the proof being worked on.
- **A grinder never needs a key.** The work happens before signing, and the signature
  is applied afterwards by the holder of the key. There is nothing to steal.

So the mechanism a grind market would need already exists. A third party can be handed
an unsigned transaction, can perform minutes of work on it, and can return a nonce that
the sender verifies and signs over.

Extending the pre-image to cover the witness was considered and rejected: third-party
witness malleability would then invalidate other people's proofs. This is the same
reason Bitcoin commits to txid rather than wtxid.

## 2. What does not exist

**There is no reason for anyone to grind a stranger's transaction.** No in-protocol
reward exists for the work, so a grind market requires a payment mechanism designed
from nothing. Every route to one is a consensus change:

- **Pay the grinder a fee.** This reintroduces fees under another name, on a chain
  whose stated premise is that there are none. See
  [feeless-transactions.md](feeless-transactions.md).
- **Mint for grind work.** This reopens the mint-safety arithmetic, which took three
  calibration rounds to settle and currently holds a 4.13x margin against α = 0.25.
  See [the difficulty floor model](../audit/mainnet-difficulty-floor-model.md).
- **Settle out of band.** Workable, but it is not a protocol feature; it is two people
  with an arrangement, and it needs nothing from v1 to exist.

The honest summary is that the mechanism is in place and the market is not.

## 3. What a GPU-less user experiences

Measured after the E28 flag day, for one honest transaction at the difficulty floor
(see [chain-growth.md](chain-growth.md) §5):

| machine | time to prove one transaction |
| --- | ---: |
| one commodity GPU | **0.83 min** |
| 8-thread CPU desktop | **15.8 min** |

The CPU figure halved at E28, from about 35 minutes. It did not become practical. A
user on a CPU-only machine can send, and will wait a quarter of an hour per
transaction, with the machine fully loaded for that time.

The desktop vault permits that CPU fallback on the live and public test
networks. The current command-line agent does not: it requires an external
solver process configured with `-cuckatoosolver=<path>` for agent spends on
those networks. The sandbox network uses its small built-in CPU solver. See the
[GPU solver guide](../gpu-solver.md) for the exact runtime boundaries.

## 4. The decision

**v1 treats a GPU as the practical transfer path and says so before work starts.**

The alternative — building a grind market for v1 — is a new sub-project with its own
brainstorm and a consensus change, and it moves launch. It was not started inside the
launch readiness program.

Where this is disclosed, all of it before the control that accepts the cost:

- [README.md](../../README.md) states the practical requirement in the project's own
  description, not in a footnote.
- The transfer screen states it above the send controls, distinguishes sandbox
  proof-of-work from the much slower live-network CPU fallback, and reports the
  result of a startup probe of the configured solver path
  ([sendcoinsdialog.cpp](../../src/qt/sendcoinsdialog.cpp)).
- The confirmation dialog repeats it.

## 5. Trigger for revisiting

A grind market becomes worth designing when there is a population that wants to
transact and cannot — that is, real users on GPU-less machines who are blocked rather
than merely inconvenienced. Until such users exist, a payment mechanism would be
designed against an imagined demand curve, and it would be paid for in consensus risk.

## 6. Related

- [chain-growth.md](chain-growth.md) — where the cost figures come from
- [transaction-pow.md](transaction-pow.md) — the mechanism being delegated
- [feeless-transactions.md](feeless-transactions.md) — why work replaces fees
- [agent-client.md](agent-client.md) — who these costs are lifted off
- [v1-scope.md](v1-scope.md) — the full list of what v1 does and does not do
