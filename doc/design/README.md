# Design

This directory contains durable Quicksilver architecture and protocol design
notes. These documents explain current consensus, policy, mining, vault, and
economic behavior at a stable design level.

Start with [what v1 does and does not do](v1-scope.md) for the scope boundary and
the trigger conditions attached to everything deliberately left out.

- [What v1 does and does not do](v1-scope.md)
- [Chain identity](chain-identity.md)
- [Economics and consensus](economics-consensus.md)
- [Block PoW with Cuckatoo](block-pow-cuckatoo.md)
- [Transaction PoW](transaction-pow.md)
- [Frame-B mint](frame-b-mint.md)
- [Tail emission](tail-emission.md)
- [Feeless transactions](feeless-transactions.md)
- [Delegated grinding](delegation.md) — decided, scoped out of v1
- [Who stores the chain](chain-storage.md) — decided; pruning is the default
- [Vault backup](vault-backup.md) — decided; encrypted history export ships in v1
- [Work-based relay](work-based-relay.md)
- [Vault mining](vault-mining.md)
- [MAX_MONEY](max-money.md)
- [Chain growth and transaction cost](chain-growth.md)
- [Network naming](network-naming.md) — implemented
- [Logging](logging.md)

## Product and client implementation status

These documents distinguish implemented behavior from remaining integration
work. Their status blocks are part of the specification; do not read every
section as a claim that the described end state already ships.

- [Agent client](agent-client.md) — core and command-line workflows implemented;
  remaining integration gaps are listed in the document
- [Desktop application](desktop-application.md) — substantial Qt implementation
  landed; remaining thin-vault work is listed in the document
- [Libraries](libraries.md) — source library map and dependency graph

## Open problems

None. Every question that was open here carries a written decision with a date and a
trigger condition:

- **Chain growth and transaction cost** — answered and shipped; see
  [chain-growth.md](chain-growth.md).
- **Who stores the chain** — decided 2026-08-04; see [chain-storage.md](chain-storage.md).
- **A market for delegated grinding** — decided 2026-08-04, scoped out of v1; see
  [delegation.md](delegation.md).
- **Whether block capacity has to scale** — deferred 2026-08-04 with a measured trigger;
  see [v1-scope.md](v1-scope.md) §3.
- **Vault backup** — decided and built 2026-08-04; see [vault-backup.md](vault-backup.md).
- **UTXO coordination between vault and agent** — decided and built 2026-08-04; see
  [agent-client.md](agent-client.md).
- **Attracting agents, and the Agent Allotment Gateway's scope** — decided 2026-08-04; see
  [v1-scope.md](v1-scope.md) §1 and §2.

Nothing in this project is described as undesigned without a document saying so
deliberately. If something belongs here again, it needs a decision, a date and a trigger
before it is written down as open.
