# Transaction Relay Policy

**Policy** (Relay Pool or Transaction Relay Policy) is the node's set of validation rules, in addition
to consensus, enforced for unconfirmed transactions before submitting them to the relay pool. These
rules are local to the node and configurable, see "Node relay options" when running `-help`.
Policy may include restrictions on the transaction itself, the transaction
in relation to the current chain tip, and the transaction in relation to the node's relay pool
contents. Policy is *not* applied to transactions in blocks.

This documentation is not an exhaustive list of all policy rules.

- [Relay Pool Limits](relaypool-limits.md)
- [Relay Pool Replacements](relaypool-replacements.md)
- [Packages](packages.md)
